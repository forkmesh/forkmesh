# Self-deploy: the cores deploy phone-approved merges

`deploy/core/self_deploy.py` runs on every core as `fmnext-self-deploy.service`
(rendered by `add_self_deploy_files` in `deploy/core/render.py` when
`[self_deploy] enabled = true` and `approver_keys` is not empty). It deploys a
merge to `main` that an enrolled phone signed, so a merge from the phone ships
while the operator desktop is offline. The App records each approval in
`deploy_approvals` (`app/src/deploy_approvals.py`, migration 0249); the phone
side is in [../native/RUNNER_AGENTS.md](../native/RUNNER_AGENTS.md) ("Merge
counts as deploy approval").

Each core polls rqlite every 15 s, checks the approval's Ed25519 signature
against `approver_keys`, its age (two hours), that the merged commit is the
approved one and is on `main`, then works through the merge in `order`
(`n1`, `n2`, `n3`): a core activates only after every core before it reports
`deployed` or `skipped`, and a failure on one core stops the rest. Per-core
progress is in `deploy_approval_nodes`; the outcome is announced to the
operators and the approver (`announce`).

## What a core does with a merge

`Agent.plan` diffs the merge against what the core runs:

| Change | What the core does |
| --- | --- |
| App release paths (same rules as `tools/next_autodeploy.py`) | Builds the release under `fmnext-build` (`systemd-run --scope`, `build_memory_max`), uploads it locally, `n1` migrates rqlite, then `activate_script` swaps `current` inside an IDS window. |
| Go daemon: `server/` (not tests, not Markdown) and the files `next_fleet.py install --steps daemon` ships (`DAEMON_TOOLS`, which includes `tools/mirror_gateway.py`) | Rolls the daemon itself (below). |
| Any other roll path (`deploy/core/`, not Markdown) | Nothing: unit, firewall, WireGuard and Caddy files are rendered from the operator's `fleet.toml` and `fleet-state.json` and still need `next_fleet.py roll` from the desktop. The core's row says "deploy/core changes need a roll from the desktop". |
| Nothing served | `skipped`, "nothing served changed". |

## Daemon and gateway roll (2026-10-08)

Asked from the phone: "share secrets from desktop to mobile via the vault and
then we can do those things from mobile". Rather than copying the fleet SSH
key to the phone, the cores roll the daemon themselves: the phone's signature
is the only thing that crosses, and no secret leaves the desktop. Before this,
a daemon or gateway change was `skipped` with "daemon or gateway changes need
a roll from the desktop" and waited for the desktop's auto-deploy.

1. **Build.** In the same checkout as the release, as `fmnext-build`, under
   its own scope (`daemon_build_memory_max`, default 700M, swap allowed,
   `GOMAXPROCS=1`, `GOGC=25`): `go build -trimpath -buildvcs=false` of
   `server/cmd/forkmesh-mirror-node` with `GOTOOLCHAIN=local` and
   `GOPROXY=off`. `server/` uses the standard library only, so nothing is
   downloaded. The cores need `golang-go`, which is now in the core
   `APT_PACKAGES`; run `next_fleet.py install nN --steps base --execute` once
   per core. Until then the core reports "daemon or gateway changes need a
   roll from the desktop (no Go toolchain on this core)". `go` in
   `[self_deploy]` overrides `/usr/bin/go`; `roll_daemon = false` turns the
   self-roll off.
2. **Pre-check.** The new binary runs `--check` as `forkmesh-node` against
   the core's own `mirror-node.json` with only `version` updated (from
   `desktop/CMakeLists.txt`, as the desktop roll does). A refusal changes
   nothing on the core.
3. **Swap.** Inside an IDS window over the binary, `/usr/local/share/forkmesh`
   and `/etc/forkmesh` (`daemon_ids_manifest`), under `/run/fmnext-activate.lock`, the agent
   saves the binary, config and tools to `/var/lib/fmnext-self-deploy/daemon-prev/`,
   installs the new ones and restarts `forkmesh-mirror-node.service`.
4. **Health.** The unit must stay `active` with an unchanged `NRestarts` for
   20 s within 90 s. Otherwise the saved files go back, the unit restarts, and
   the core reports "daemon: unhealthy after the roll (...); rolled back",
   which fails the approval and stops the remaining cores.

An app change and a daemon change in one merge build both, then activate the
app first and roll the daemon second.

Differences from `next_fleet.py roll`:

- **No DNS drain.** Draining needs the Cloudflare token, which stays on the
  desktop ([../VAULT.md](../VAULT.md)). The restart takes a few seconds, the
  other cores keep serving, and only one core restarts at a time.
- **The config is not re-rendered.** The overrides, router key and gateway
  settings stay as the last desktop roll left them; only the binary, its
  tools and `version` change.
- **Runners are not rolled.** A deployed core says "daemon rolled on the
  cores; runners wait for the desktop". The desktop auto-deploy counts roll
  paths since its last roll ([auto-deploy.md](auto-deploy.md)), so the next
  time it runs it rolls the runners, and rolls the cores again with the same
  code.

The core records what it rolled in `/var/lib/fmnext-self-deploy/daemon-rev`.
The next self-roll diffs from it; with no record, the core takes the app
revision it runs as the starting point. A desktop roll after a self-roll
leaves the record behind, which can only cause one extra roll of the same
code.
