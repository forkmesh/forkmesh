# Tools (`tools/`)

`tools/` holds scripts that belong to no single app: fleet build, deploy and
operations, release and packaging, gateway files the cores run, protocol and
evidence ledgers, agent helpers, and the projects-layout resolver. Most are
standard-library Python run from the repository root with
`python3 tools/<name>.py --help`. App-specific tools live next to their app
(`app/tools/`, `world/tools/`, `marketplace/tools/`, `desktop/scripts/`,
`native/ios/tools/`). Fleet tools run on the operator machine; those that
change something are dry runs unless given `--execute`.

## Code map

**Fleet and releases** ([../deploy/README.md](../deploy/README.md))

| File | What it does |
| --- | --- |
| `next_common.py` | Shared plumbing: fleet loading, SSH (BatchMode, pinned `known_hosts`), the operator config dir, env-file reads that never echo |
| `next_fleet.py` | Cloud instances, node install steps, secrets push, destroy, core replacement, core and runner rolls (`roll`, `roll-runners`), `delete-refs` |
| `next_cloud.py`, `next_aws.py` | Cloud provider interface; the AWS EC2 provider (standard-library SigV4) |
| `next_deploy.py` | App release build, upload, migrate, rolling restart, rollback |
| `next_autodeploy.py` | Deploys `main` to the core nodes when served-stack paths change ([auto-deploy](../deploy/auto-deploy.md)) |
| `next_autoscale.py` | Operator side of the core autoscaler ([AUTOSCALING.md](../fleet/AUTOSCALING.md)) |
| `next_vault.py` | Secrets vault: `status`, `sync`, `unseal`, `seal`, `reload` ([VAULT.md](../VAULT.md)) |
| `next_ids.py` | IDS: `keygen`, `fingerprint`, `status`, `baseline`, `expect`, `test-fire`, `release` ([IDS.md](../IDS.md)) |
| `next_dns.py` | DNS for the served names and core node names, plus DNS health failover |
| `next_mail.py` | Core mail: DKIM, DNS, reverse DNS, install, check ([MAIL_LOG.md](../MAIL_LOG.md)) |
| `next_node_owner.py` | Fleet node ownership ([NODE_OWNERSHIP.md](../NODE_OWNERSHIP.md)) |
| `next_backup.py` | Nightly age-encrypted rqlite backup: `run`, `prune`, `keygen`, `verify`, `restore` |
| `next_rekey.py` | Offline re-key of a database copy ([REKEY.md](../REKEY.md)) |
| `next_git_host.py`, `next_codemagic.py` | Private Git over HTTPS on the cores; native iOS builds cloned from it (full history: dumb HTTP has no shallow clones) |
| `rqlite_migrate.py` | Applies `app/migrations/*.sql` to rqlite, keeping the `d1_migrations` ledger shape |
| `source_publisher.py` | Publishes `forkmesh/forkmesh` from a checkout without the desktop ([source-publisher.md](source-publisher.md)) |
| `prune_merged.py` | Prunes merged branches, worktrees and finished agent sessions ([prune-merged.md](prune-merged.md)) |
| `branch_inventory.py` | Classifies unmerged branches and saves daily snapshots ([branch-inventory.md](branch-inventory.md)) |
| `cutover_*.py`, `rehost.py` | Hostname and cutover tooling ([CUTOVER.md](../CUTOVER.md)) |

**Paths and layout** ([projects-layout.md](projects-layout.md)): `forkmesh_paths.py`
(Python twin of `desktop/src/AppPaths.{h,cpp}`), `forkmesh-paths.sh` (the
same for shell scripts), `forkmesh_layout.py` (`plan` or `apply`).

**Release and packaging** ([../release/README.md](../release/README.md)):
`forkmesh-release-publish.sh`, `forkmesh-release-host.py` (assets only the
release host can build), `forkmesh-release-promote.py`,
`forkmesh_release_request.py` ([mcp-releases.md](mcp-releases.md)),
`package/` (native installers per platform, rootless toolchains, appcast),
`clean_machine_*` (install transcript in a disposable VM), `openssl-local.sh`,
`qt64-build-check.sh`, `android-toolchain.sh`.

**Gateways** ([../server/README.md](../server/README.md)): `mirror_gateway.py`,
`direct_https_bootstrap.py`, `node_operator_policy.py` and `secret_gate.py`
run on the cores next to the Go daemon (`DAEMON_TOOLS` in `next_fleet.py`).
Also here: the SSH gateway and authorization broker, sshd and fail2ban
hardening scripts, Cloudflare Tunnel bootstrap, headless mirror helpers and
security scan runners.

**Agents and desktop helpers**: `forkmesh_coding_agent.py` and
`forkmesh_agent_guard.py` (embedded into the desktop by their
`sync_*_header.py` scripts; edit the Python, then regenerate),
`forkmesh_mcp_server.py` ([../agents/tools.md](../agents/tools.md#forkmesh-mcp-server)),
`branch_pr_review.py` (loopback-only diff viewer),
`pull_ledger.py` (comment on, close and reopen a native pull from the terminal, [pull-ledger.md](pull-ledger.md)), `code_health.py`,
`measure_startup.py`, `run_critical_tests.py`, `quality_gate.py`.

**Evidence ledgers and contracts**: each backs a `www/docs` page; inputs and
outputs are listed in [../conformance/README.md](../conformance/README.md).

**Other**: forge sync and imports (`forge_sync.py`, `forge_providers.py`,
`github_*`, `hosted_repository_sync.py`), legacy reward and custody tools,
`ecosystem_seed.py` (signed `POST /api/ecosystem/import`,
[../app/ecosystem.md](../app/ecosystem.md)).

## Architecture

There is no shared framework. Tools that touch ForkMesh files resolve paths
through `forkmesh_paths` or `forkmesh-paths.sh`, never hard-coded folders.
Fleet tools share `next_common.py` and read `deploy/fleet.toml`; they never
render or print secret values: secrets are read from files in the operator
config dir and piped over SSH stdin. Nodes are reached over SSH (BatchMode,
pinned `known_hosts`) and rqlite through an SSH tunnel.

## Configuration

- `FORKMESH_PROJECTS_DIR`, `FORKMESH_HOME`, `FORKMESH_PROJECT`,
  `FORKMESH_DATA_DIR`, `FORKMESH_CONFIG_DIR`: see [projects-layout.md](projects-layout.md).
- `FORKMESH_TOOLCHAIN_DIR`: toolchain root for `android-toolchain.sh`.
- `tools/requirements-ci.txt`: Python packages for CI runs.
- `FMNEXT_CONFIG_DIR`: the operator config dir (default
  `~/projects/forkmesh/config/fleet`): `secrets.env`, provider env files,
  `fleet-state.json`, `known_hosts`, `DATA_KEY`, the backup age key and the
  IDS operator key. Back the keys up offline.
- `FMNEXT_DATA_DIR`, `FMNEXT_LOGS_DIR`, `FMNEXT_SSH_KEY`, `FMNEXT_DIST_DIR`:
  overrides for the fleet data, logs, SSH key and build output.

## Build, run, test

```sh
python3 tools/forkmesh_paths.py                 # show where files resolve
python3 tools/run_critical_tests.py             # bounded critical suites
python3 -m pytest tools/test_*.py
python3 -m pytest deploy/tests                  # the fleet tools
python3 tools/next_fleet.py list                # read-only
```

Many tools are also covered by `app/tests/test_<tool>.py`. The fleet runbook
is [OPERATIONS.md](../OPERATIONS.md).

## Security

- `forkmesh_agent_guard.py` fails closed: a path outside the allowlist is
  refused.
- `branch_pr_review.py` serves on loopback only.
- Tools never print secrets; tool secrets live under `config/tools/secrets`.
- `next_fleet.py` refuses any instance whose label or tags do not mark it as
  part of the fleet (`instance_label_prefix` in `deploy/fleet.toml`).
- `next_dns.py` refuses names outside the served and fleet zones, record
  types other than A and AAAA, and a token file readable by group or other.
- Never erase `~/projects/forkmesh/{data,config,ssh,logs}/fleet` (`AGENTS.md`).

## Traps

- A test that runs an installer or layout tool must sandbox `HOME` and
  `XDG_*_HOME` and drop `FORKMESH_HOME` / `FORKMESH_PROJECTS_DIR`
  (`AGENTS.md`; `app/tests/conftest.py`). An `XDG_*_HOME` pointing away from
  its default is treated as a deliberate sandbox by `forkmesh_paths`.
- A home folder that is still a git checkout keeps every root on the legacy
  paths, so application data never lands where `git add -A` reaches it.
- `forkmesh_layout.py apply` refuses to run while the desktop, its services,
  Actions runs or git operations are live.
- `test_ci_failure_quarantine_is_explicit_and_bounded` pins the exact number
  of quarantined test ids in `app/tests/conftest.py` and
  `world/tests/conftest.py`; update the count in the same commit.
- `deploy/`, `app/host/`, `tools/next_*` and `tools/cutover_*` keep the
  fleet's own zone names for the core nodes, so `rehost.py --check` reports
  hosts to rewrite.
