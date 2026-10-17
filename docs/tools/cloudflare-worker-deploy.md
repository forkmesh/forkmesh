# Cloudflare Worker deploys from ForkMesh Actions

`tools/cloudflare_worker_deploy.py` lets a user mirror an existing Worker
repository on ForkMesh and deploy it with Wrangler from their own desktop.
User guide: [www/docs/cloudflare-workers.md](../../www/docs/cloudflare-workers.md).
Tests: `app/tests/test_cloudflare_worker_deploy.py`.

## Pieces

| Subcommand | Does |
| --- | --- |
| `connect` | Reads `wrangler.jsonc` / `wrangler.json` / `wrangler.toml`, writes `.forkmesh/cloudflare-deploy.json` (`forkmesh-cloudflare-deploy-v1`), copies itself to `.forkmesh/cloudflare-deploy.py`, renders `.forkmesh/deploy-cloudflare.yml` and `.forkmesh/rollback-cloudflare.yml`, and lists GitHub workflows that already deploy. `--github` also runs `github_forkmesh_bridge.connect` from the same folder |
| `check [--online]` | Config, Wrangler file and environment, Node.js, address space; online: token (`/user/tokens/verify`, then the account-token route), `account_id` pin, whether the Worker exists |
| `deploy [--dry-run]` | Installs with the lockfile's package manager, runs `wrangler deploy --config … [--env …] --keep-vars --message "ForkMesh <commit>"`, masks the token, prints `::cloudflare-deploy:: ok worker=… version=… url=…` |
| `rollback [--version]` | `wrangler rollback`; its confirmation defaults to yes, so it does not stop for a prompt in a run |

The file is standard library only and self-contained, like the GitHub bridge,
because `connect` copies it into the user's repository.

## Why it is shaped this way

- **Runs on the owner's desktop.** Mirrors and shared runners hold no
  Actions variables and have no network ([../server/actions-runner.md](../server/actions-runner.md),
  [../server/runner-actions.md](../server/runner-actions.md)); only the
  desktop's `ActionRunner` exposes `${{ vars.X }}` from Settings → Secrets and
  honours `FORKMESH_ACTIONS_ALLOW_NETWORK`.
- **`runs-on: cloudflare-deploy`.** Desktop labels are the node name, the
  served-mirror name, the OS and Settings → Profile → Node labels
  (`ActionFile::nodeLabels`). Mirrors match `linux`/`mirror` and runners match
  `linux`/`runner`, so a dedicated label is what keeps the run off them.
- **Secrets go through top-level `env:`.** The desktop parser reads `env:`
  only at the top level and substitutes `${{ vars.X }}` there; step commands
  carry no secret text. `secrets.X` and `inputs.X` are not substituted.
- **The branch check is in `deploy`.** The desktop ignores `on.push.branches`
  and fires `on: push` for the mirror's HEAD branch, while a dispatch can name
  any branch, so `deploy` skips any `FORKMESH_REF` that is not the configured
  branch.
- **`--keep-vars` by default.** An existing Worker often has variables set in
  the dashboard; a plain `wrangler deploy` deletes those not in the config.
- **Node.js 22 and 32 GiB of address space.** Wrangler 4.151 declares
  `node >= 22`. On Node 22 its WebAssembly modules reserve a sparse range
  that failed at a 24 GiB `RLIMIT_AS`. The desktop now grants at least 64 GiB
  (`actionAddressSpaceBytes`); the check catches an older desktop before the
  install starts.
- **Phones.** The iOS and Android Actions screens drive desktop runs over
  local control (`repos.prepare` / `repos.run`: `workflow.run`, `run.rerun`,
  `run.stop`, `run.approve`), so both generated workflows declare
  `workflow_dispatch`. The web Actions tab only controls schema-v2 mirror
  executors and cannot start desktop runs.

## Traps

- `FORKMESH_ACTIONS_ALLOW_NETWORK` is a desktop-wide variable: it opens the
  network for every Actions run on that desktop, not just this workflow.
- With the GitHub bridge, anyone with GitHub write access can change the
  workflow; per-repository auto-approve is on by default, so the guide tells
  users to turn it off.
- `repos.prepare` and `startManualWorkflowRun` do not check
  `allowsManualRun()`; only the desktop UI and the phone apps hide dispatch for
  push-only workflows.
