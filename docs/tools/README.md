# Tools (`tools/`)

`tools/` holds the scripts that are not part of any one app: the fleet tools
that build, deploy and operate the core nodes and the runner tier, release
and packaging, setup for the mirror hosts (all now deleted) and the gateway
files the cores still run, protocol and evidence ledgers, agent helpers, and
the projects-layout resolver. Most are standard-library Python and run from
the repository root with `python3 tools/<name>.py --help`. Tools that belong
to one app live next to it instead (`app/tools/`, `world/tools/`,
`marketplace/tools/`, `desktop/scripts/`, `native/ios/tools/`). The fleet tools
run on the operator desktop; those that change something are dry runs unless
given `--execute` (`next_import.py` takes `--dry-run` instead).

## Code map

**Fleet and releases** (see [docs/deploy/README.md](../deploy/README.md))

| File | What it does |
| --- | --- |
| `tools/next_common.py` | Shared plumbing: fleet loading, SSH (BatchMode, pinned `known_hosts`), the operator config dir, env-file reads that never echo |
| `tools/next_fleet.py` | Vultr and AWS instances, node install steps, secrets push, destroy, core replacement, core and runner rolls (`roll`, `roll-runners`) |
| `tools/next_cloud.py`, `tools/next_aws.py` | Cloud provider interface; the AWS EC2 provider (standard-library SigV4) ([../deploy/README.md](../deploy/README.md#cloud-providers)) |
| `tools/next_deploy.py` | App release build, upload, migrate, rolling restart, rollback |
| `tools/next_autodeploy.py` | Deploys `main` to the core nodes when served-stack paths change: `serve`, `status`, `pause`, `resume`, `disarm`, `deploy`, `rollback`, `verify`, `install` ([auto-deploy.md](../deploy/auto-deploy.md)) |
| `tools/next_autoscale.py` | Operator side of the autoscaler that runs on the cores: signed settings, manual scale, kill switch, boot snapshot build ([AUTOSCALING.md](../fleet/AUTOSCALING.md)) |
| `tools/next_vault.py` | Secrets vault: `status`, `sync`, `unseal`, `seal`, `reload` ([VAULT.md](../VAULT.md)) |
| `tools/next_ids.py` | IDS: `keygen`, `fingerprint`, `status`, `baseline`, `expect`, `test-fire`, `release` ([IDS.md](../IDS.md)) |
| `tools/next_dns.py` | Cloudflare DNS for `app.forkmesh.com`, `world.forkmesh.com` and names under `next.forkmesh.com` (the core node names): `status`, `ensure`, and the DNS health failover (`health-once`, `health-loop`, `drain`, `undrain`) |
| `tools/next_mail.py` | Core mail: DKIM, DNS, reverse DNS, install, restart, check ([MAIL_LOG.md](../MAIL_LOG.md)) |
| `tools/next_node_owner.py` | Fleet node ownership: `status`, `register`, `transfer` ([NODE_OWNERSHIP.md](../NODE_OWNERSHIP.md)) |
| `tools/next_backup.py` | Nightly rqlite backup to the operator box, age-encrypted: `run`, `prune`, `keygen`, `encrypt-existing`, `verify`, `restore` |
| `tools/next_rekey.py` | Offline re-key of a database copy ([REKEY.md](../REKEY.md)) |
| `tools/next_git_host.py`, `tools/next_codemagic.py` | Private Git over HTTPS on the cores; Codemagic native iOS builds cloned from it |
| `tools/next_import.py` | Rebuilds Next's history on top of `c48046db2` as the branch `next-import` ([NEXT_ADOPTION.md](../NEXT_ADOPTION.md)) |
| `tools/rqlite_migrate.py` | Applies `app/migrations/*.sql` to rqlite, keeping wrangler's `d1_migrations` ledger shape |
| `tools/status_history_repair.py` | Recomputes `/status` hour and day batches that lost coverage ([CUTOVER.md](../CUTOVER.md)) |
| `tools/source_publisher.py` | Publishes `forkmesh/forkmesh` from the checkout without the desktop: fast-forward sync into the bare mirror, signed HTTPS pushes to the cores through `app.forkmesh.com`, and the flagship guard. See [source-publisher.md](source-publisher.md). |
| `tools/prune_merged.py` | Prunes merged branches, their worktrees and finished agent sessions; `--remote` also deletes them on the cores ([prune-merged.md](prune-merged.md)) |
| `tools/branch_inventory.py` | Sorts every unmerged branch into merged, ready, behind, conflicted, stale or orphaned and saves a dated snapshot for day-over-day trends ([branch-inventory.md](branch-inventory.md)) |

**Hostnames and host shim**

| File | What it does |
| --- | --- |
| `tools/rehost.py` | Rewrote production hostnames to `next.forkmesh.com` names while the fleet was built (`--check`, `--apply`, `--flags`) |
| `tools/js_surface_inventory.py` | Lists the Worker-runtime surface (`js`, `pyodide.ffi`, `workers`, the `env`/`ctx` bindings) the app code uses, so a test fails when it uses something the CPython host (`app/host/forkmesh_host`) does not implement |

**Cutover** ([CUTOVER.md](../CUTOVER.md))

`tools/cutover_data.py`, `tools/cutover_dns.py`, `tools/cutover_identity.py`,
`tools/cutover_rehost.py` (the inverse of `rehost.py`; refuses a `main`
checkout), `tools/cutover_tls.py`.

**Paths and layout**

| File | What it does |
| --- | --- |
| `tools/forkmesh_paths.py` | Resolves every ForkMesh folder (data, config, cache, state, logs, ssh, source, toolchain, release, publisher, fleet). Python twin of `desktop/src/AppPaths.{h,cpp}`. `python3 tools/forkmesh_paths.py` prints the resolution. See [projects-layout.md](projects-layout.md). |
| `tools/forkmesh-paths.sh` | The same resolution for shell scripts (source it). |
| `tools/forkmesh_layout.py` | Moves a box onto the projects layout. `plan` (default, read-only) or `apply`. |

**Release and packaging** (see [`docs/release/README.md`](../release/README.md))

| File | What it does |
| --- | --- |
| `tools/forkmesh-release-publish.sh` | Publishes prebuilt binaries without committing them. |
| `tools/forkmesh-release-host.py`, `tools/release-host/forkmesh-release-host.{service,timer}` | Builds the assets only the release host can build (Windows installer, APK, macOS `.dmg` via Codemagic), completes a release, and seeds release blobs to the cores in `config/fanout/release-hosts`. |
| `tools/forkmesh-release-promote.py` | Promotes a signed candidate channel to the public channel. |
| `tools/forkmesh-release-android.py` | Compatibility entry point for the Android release step. |
| `tools/forkmesh_release_request.py` | Asks the account's desktop to cut (`create`) or publish a release from any terminal, signed with this device's key ([mcp-releases.md](mcp-releases.md)). |
| `tools/package/package-release.sh` | Builds a native installer from a built executable. Per platform: `appimage.sh`, `macos.sh`, `windows.sh` + `forkmesh.nsi`, `nsis-uninstall-manifest.sh`. |
| `tools/package/*-toolchain.sh`, `tools/package/install-*-toolchain.sh` | Rootless AppImage and Windows cross-build toolchains. |
| `tools/package/generate-appcast.sh` | Sparkle-format appcast from a channel's `release.json` (no shipped desktop reads it). |
| `tools/clean_machine_transcript.sh`, `tools/clean_machine_vm.py`, `tools/clean_machine_expectations.json` | Clean-machine install transcript in a disposable QEMU VM (#705). |
| `tools/openssl-local.sh` | Builds a SHA-256-pinned OpenSSL 3.5 into the toolchain directory for `-DFORKMESH_BUNDLED_OPENSSL=ON`. |
| `tools/qt64-build-check.sh` | Builds `desktop/` against the Qt 6.4.2, GCC and OpenSSL 3.0 of Ubuntu 24.04 / Debian 12. |
| `tools/android-toolchain.sh`, `tools/loopback-netns.sh` | Rootless Android SDK and JDK for the native Android Gradle build (reports `ANDROID_TOOLCHAIN_READY` / `ANDROID_TOOLCHAIN_STATUS` / `ANDROID_TOOLCHAIN_REASON`); run Gradle when IPv4 loopback is broken. |

**Mirror hosts and gateways** (see [`docs/server/README.md`](../server/README.md))

These set up and ran the mirror hosts. Every mirror host is deleted: the
Cloudflare-Tunnel mirrors (mirror2–34) and the production mirrors
(mirror35–38, retired 2026-09-24); their roles run on the cores and the runner
`fmnext-r1` ([MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)). The cores still
run `mirror_gateway.py`, `direct_https_bootstrap.py`, `node_operator_policy.py`
and `secret_gate.py` next to the Go daemon (`server/`), their Git gateway
(`DAEMON_TOOLS` in `tools/next_fleet.py`).

| File | What it does |
| --- | --- |
| `tools/harden_mirror_sshd.sh` | Locks a mirror's sshd to key logins. |
| `tools/mirror_fail2ban.sh` | Bans addresses that keep failing SSH on a mirror. |
| `tools/provision_ssh_gateway.sh`, `tools/ssh_gateway.py`, `tools/ssh_authorization_broker.py`, `tools/ssh_post_receive_refresh.py`, `tools/sign_ssh_host_certs.sh` | Public read-only Git SSH gateway, its broker, post-receive bridge, and a shared host certificate. `ssh.forkmesh.com` is retired. |
| `tools/mirror_gateway.py`, `tools/direct_https_bootstrap.py` | Local HTTPS-origin gateway; direct HTTPS listener and DNS. |
| `tools/cloudflare_bootstrap.py`, `tools/cloudflare_tunnel_bootstrap.py`, `tools/cloudflared_install.py` | Independent App on Cloudflare (Worker, D1, route); Cloudflare Tunnel exposure of a mirror gateway; pinned `cloudflared`. |
| `tools/headless_mirror_identity.py`, `tools/headless_mirror_refresh.py` | Headless mirror identity and refresh. |
| `tools/native_security_scan_runner.py`, `tools/security_scan.py`, `tools/secret_gate.py` | Daily mirror security scan; public-safe report; secret scanning at replication boundaries. |
| `tools/node_operator_policy.py` | What one node stores, serves and removes. |

**Agents and desktop helpers**

| File | What it does |
| --- | --- |
| `tools/forkmesh_coding_agent.py` | Provider-neutral coding agent used by the desktop. Embedded into the desktop by `tools/sync_coding_agent_header.py`. |
| `tools/forkmesh_agent_guard.py` | Fail-closed filesystem allowlist for coding agents. Embedded by `tools/sync_agent_guard_header.py`. |
| `tools/forkmesh_mcp_server.py` | MCP server exposing narrow mesh capabilities, including the organization task board (`docs/app/task-progress.md`), the organization CRM (`crm_*`: pipeline reads, one-stage contact moves with dated evidence notes, custom-column edits, `crm_find_duplicates`, `crm_check_accounts` (ForkMesh administrators: records account, signup and membership evidence on contacts), and `crm_merge_contacts`, which needs an evidence note and is flagged destructive because it deletes the duplicate; see `marketplace/packages/crm/README.md`), `create_agent`, which adds agents to the desktop's Agents list ([mcp-create-agent.md](mcp-create-agent.md)), and `show_window`/`read_window`/`close_window`, small desktop windows an agent opens to show results or ask the user, and `open_browser_window`/`read_browser_page`, a small browser window whose page the agent can read ([mcp-agent-windows.md](mcp-agent-windows.md)), and `list_releases`/`create_release`/`publish_release`, which cut and publish releases with no dialog ([mcp-releases.md](mcp-releases.md)). |
| `tools/branch_pr_review.py` | Local diff viewer for `port/*` branches with PR creation (loopback only). |
| `tools/mainwindow_split/` | libclang tooling that split `desktop/src/MainWindowInternal.h` (2026-09-22): `mw_refs.c`, `run_refs.py`, `summarize.py`, `inline_defs.c`, `move_inline_defs.py`, `split_internal_header.py`. |
| `tools/code_health.py`, `tools/measure_startup.py` | Desktop code health report; startup-to-first-paint timing. |
| `tools/run_critical_tests.py` | Bounded critical pytest suites for local runs and CI. |
| `tools/quality_gate.py` | Auditable completion gates (`www/docs/engineering/quality-gates.json`). |

**Evidence ledgers and contracts** (each backs a `www/docs` page; see [`docs/conformance/README.md`](../conformance/README.md))

`tools/git_conformance.py`, `tools/dependency_conformance.py`,
`tools/category_comparisons.py`, `tools/protocol_certification.py`,
`tools/protocol_governance.py`, `tools/public_interest_funding.py`,
`tools/external_dependency_inventory.py`, `tools/forkmesh_integration.py`,
`tools/forkmesh_service_roles.py`, `tools/social_signals.py`,
`tools/forkmesh_trust_lists.py`, `tools/forkmesh_transports.py`,
`tools/forkmesh_sync_status.py`, `tools/forkmesh_availability_proof.py`,
`tools/forkmesh_availability_contract.py`, `tools/forkmesh_crisis_preservation.py`,
`tools/forkmesh_disaster_recovery.py`, `tools/forkmesh_offline_sync.py`,
`tools/verify_collaboration.py`, `tools/verify_migration_report.py`,
`tools/outcome_metrics.py`, `tools/unit_economics.py`, `tools/terminology_lint.py`.

**Forge sync and imports**: `tools/forge_sync.py`, `tools/forge_providers.py`,
`tools/github_forkmesh_bridge.py`, `tools/github_issue_sync.py`,
`tools/hosted_repository_sync.py`.

**Rewards and custody (legacy)**: `tools/reward_scheduler.py`,
`tools/validate_reward_mainnet.py`, `tools/legacy_solana_custody.py`.

**One-off operations**: `tools/audit_private_surfaces.py` (inspect a D1
SQLite backup and private replicas without decryption keys),
`tools/rewrite_commit_messages.py` (history rewrite; see
`www/docs/operations/commit-history-rewrite.md`), `tools/ecosystem_seed.py`
with `tools/ecosystem_third_parties.json` and `tools/ecosystem_requested.json` (load a ForkMesh Ecosystem JSON
file through the signed `POST /api/ecosystem/import`; see
[docs/app/ecosystem.md](../app/ecosystem.md)).

## Architecture

No shared framework across `tools/`. Tools that touch ForkMesh files resolve
paths through `forkmesh_paths` (Python) or `forkmesh-paths.sh` (shell) rather
than hard-coding `~/.forkmesh` or XDG folders. Tools the desktop embeds
(`forkmesh_coding_agent.py`, `forkmesh_agent_guard.py`) are copied into C++
headers by their `sync_*_header.py` script; edit the Python, then regenerate.

The fleet tools share `next_common.py` and read `deploy/fleet.toml`. They
never render or print secret values; secrets are read from files in the
operator config dir and piped over SSH stdin.

## Protocols and interfaces

- Each ledger tool's input and output files are listed in
  [`docs/conformance/README.md`](../conformance/README.md).
- The fleet tools reach the nodes over SSH (BatchMode, pinned
  `known_hosts`, the fleet key) and rqlite through an SSH tunnel.
- `source_publisher.py` signs each push request `forkmesh-git-push-v1`
  ([source-publisher.md](source-publisher.md)).

## Configuration

- `FORKMESH_PROJECTS_DIR`, `FORKMESH_HOME`, `FORKMESH_PROJECT`,
  `FORKMESH_DATA_DIR`, `FORKMESH_CONFIG_DIR`: see [projects-layout.md](projects-layout.md).
- `FORKMESH_TOOLCHAIN_DIR`: toolchain root for `android-toolchain.sh`.
- `tools/requirements-ci.txt`: Python packages for CI runs.
- `FMNEXT_CONFIG_DIR`: the operator config dir (default
  `~/projects/forkmesh/config/fleet`): `secrets.env`, `cloudflare.env`,
  `cloudflare-dns.env`, `vultr.env`, `aws.env`, `fleet-state.json`,
  `known_hosts`, `DATA_KEY`, the backup age key (`backup-age.key`) and the IDS
  operator key (`ids-operator.key`). Back the keys up offline.
- `FMNEXT_DATA_DIR`, `FMNEXT_LOGS_DIR`, `FMNEXT_SSH_KEY`: override the fleet
  data dir, logs dir and SSH key; by default `forkmesh_paths` resolves them to
  `~/projects/forkmesh/{data,logs}/fleet` and
  `~/projects/forkmesh/ssh/fleet/next_fleet_ed25519` (`AGENTS.md`).
- `FMNEXT_DIST_DIR`: release build output.

## Build, run, test

```sh
python3 tools/forkmesh_paths.py                 # show where files resolve
python3 tools/run_critical_tests.py             # bounded critical suites
python3 -m pytest tools/test_*.py
python3 -m pytest deploy/tests                  # the fleet tools
python3 tools/next_fleet.py list                # read-only
```

Many tools are also covered by `app/tests/test_<tool>.py`.

## Operations

- [OPERATIONS.md](../OPERATIONS.md) is the runbook for the fleet tools.
- Release: [`docs/release/README.md`](../release/README.md).
- The Go daemon the cores run as their Git gateway (the mirror hosts it also
  ran on are deleted): [`docs/server/README.md`](../server/README.md).
- Source publisher: [source-publisher.md](source-publisher.md).

## Security

- `tools/forkmesh_agent_guard.py` fails closed: a path outside the allowlist is
  refused.
- `tools/branch_pr_review.py` serves on loopback only
  (`tools/test_branch_pr_review_guard.py`).
- Tools never print secrets; `forkmesh_paths` puts tool secrets under
  `config/tools/secrets` in the project folder.
- `next_fleet.py` refuses any Vultr instance whose label lacks the
  `fmnext-` prefix (`instance_label_prefix` in `deploy/fleet.toml`), and any
  AWS instance without `Name=fmnext-…` and `forkmesh-next=1`.
- `next_dns.py` refuses any name other than `app.forkmesh.com`,
  `world.forkmesh.com` and names under `next.forkmesh.com`, any record type
  but A and AAAA, and a DNS token file readable by group or other.
- Never erase `~/projects/forkmesh/{data,config,ssh,logs}/fleet` (`AGENTS.md`).

## Traps

- A test that runs an installer or layout tool must sandbox `HOME` and
  `XDG_*_HOME` and drop `FORKMESH_HOME` / `FORKMESH_PROJECTS_DIR`
  (`AGENTS.md`; `app/tests/conftest.py`). An `XDG_*_HOME` pointing away from its
  default is treated as a deliberate sandbox by `forkmesh_paths`.
- A home folder that is still a git checkout keeps every root on the legacy
  paths, so application data never lands where `git add -A` reaches it.
- `forkmesh_layout.py apply` refuses to run while the desktop, its services,
  Actions runs or git operations are live.
- `test_ci_failure_quarantine_is_explicit_and_bounded` pins the exact number of
  quarantined test ids in `app/tests/conftest.py` and `world/tests/conftest.py`.
  Adding or removing an entry means updating that count in the same commit; it
  runs in the first CI step, so a stale count stops every later step.
- The code carries production hostnames again (`cutover_rehost.py`, applied
  2026-09-24), but `deploy/`, `app/host/`, `tools/next_*`, `tools/cutover_*`
  and `rehost.py` keep `next.forkmesh.com` for the fleet's own zone and the
  core node names `nN.next.forkmesh.com`. `rehost.py --check` therefore
  reports hosts to rewrite; its guard test is retired.
- Codemagic clones need the full history (dumb HTTP has no shallow clones;
  3493b80).

## Recent changes

- 2026-09-30: `branch_inventory.py`, the integration inventory with a saved
  snapshot per day; it counts a branch whose merge would change nothing as
  merged ([branch-inventory.md](branch-inventory.md)).
- 2026-09-28: the quarantine count drops from 54 to 53. The Flutter removal
  deleted a quarantined `test_surface_capability_matrix.py` entry without
  updating the count, which failed every CI tests run, main included.
- 2026-09-27: `prune_merged.py` and `next_fleet.py delete-refs`, a batch
  branch delete on every core at once that repeats until nothing comes back
  ([prune-merged.md](prune-merged.md)).
- 2026-09-27: `forkmesh-paths.sh` gains `forkmesh_tool_logs_dir`
  (`~/projects/forkmesh/logs/tools`): the native Android shortcut wrote its
  emulator log through `~/.forkmesh/logs`, a dangling symlink once the layout
  moved, and stopped before starting the emulator.
- 2026-09-27: `ecosystem_third_parties.json`, every third party the code
  mentions, as public Ecosystem listings with no private notes, loaded with
  `ecosystem_seed.py`. The loader now has a lasting use: re-import the file
  when the code gains or drops a third party
  ([docs/app/ecosystem.md](../app/ecosystem.md)).
- 2026-09-26: faster push-to-fleet. `source_publisher.py` passes start 2 s
  apart instead of 30 s ([source-publisher.md](source-publisher.md)).
  `next_deploy.py` reuses one SSH connection per node, uploads to every node at
  once while the migration runs, reuses the node's venv when the wheels match,
  and caches the build wheelhouse. `rqlite_migrate.py` sends the app schema
  replay in batches of 100 ([deploy/README.md](../deploy/README.md), Deploy
  speed).
- 2026-09-26: `ecosystem_seed.py`, a one-off loader for the ForkMesh
  Ecosystem: it signs a projects JSON file with an administrator's node key and
  posts it to `/api/ecosystem/import`. The data file stays out of the
  repository because it carries private notes; the tool can be deleted once
  the directory is seeded ([docs/app/ecosystem.md](../app/ecosystem.md)).
- 2026-09-24: cutover tools (3e432a2); `next_ids.py` (6df712f);
  `next_git_host.py`, `next_codemagic.py` (068e876, 938d609); `next_rekey.py`
  (6d2cea5); age-encrypted backups (0ba6371).
- 2026-09-24: `source_publisher.py` publishes without the flagship desktop;
  `forkmesh_paths` gains publisher and fleet paths
  ([source-publisher.md](source-publisher.md)).
- 2026-09-24: `tools/package/nsis-uninstall-manifest.sh`: the Windows
  uninstaller deletes only its own payload (#727, aef0332af).
- 2026-09-23: fleet, deploy, DNS and backup tooling (0b0ec8b); `rehost.py`
  (35a9657); `rqlite_migrate.py` (a47559a).
- 2026-09-22: `mirror_fail2ban.sh`, sshd key-only; Windows OpenSSL 3.5
  cross-build; APKs signed with the v3 key; `mainwindow_split/` tooling.
- 2026-09-21: projects layout: `forkmesh_paths.py`, `forkmesh-paths.sh`,
  `forkmesh_layout.py`; install defaults to `~/projects/forkmesh/src`.
- 2026-09-17: clean-machine VM transcript with screenshots and video (#705).

## Related docs

- [docs/deploy/README.md](../deploy/README.md)
- [OPERATIONS.md](../OPERATIONS.md)
- [CUTOVER.md](../CUTOVER.md)
- [MIRROR_RETIREMENT.md](../MIRROR_RETIREMENT.md)
- [projects-layout.md](projects-layout.md)
- [source-publisher.md](source-publisher.md)
- [prune-merged.md](prune-merged.md)
- [branch-inventory.md](branch-inventory.md)
- [mcp-create-agent.md](mcp-create-agent.md)
- [`docs/conformance/README.md`](../conformance/README.md)
- [`docs/release/README.md`](../release/README.md)
