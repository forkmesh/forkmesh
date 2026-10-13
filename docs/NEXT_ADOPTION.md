# Adopting ForkMesh Next

Status: 2026-09-24. Part A is done on the `adopt-next` branch. Part B is the plan for the later folder move. Nothing has been moved yet.

## Part A: Next's code and history in this repository

ForkMesh Next (`~/projects/forkmesh-next/src`) was seeded from production main
`c48046db2` with `git archive`. Its seed commit `01450c5` equals that tree,
except that it leaves out `mobile/`, `native/`, `.forkmesh/`, `.github/`,
`codemagic.yaml`, `deploy-checklist.md` and `.mcp.json`, and adds
`UPSTREAM.md`, `docs/PLAN.md` and a section in `AGENTS.md`.

`tools/next_import.py` rebuilds Next's history on top of `c48046db2` as the
branch `next-import`:

- **Fetch.** It fetches the Next ref into `refs/next-import/source`. This is a
  local ref and is never pushed.
- **Map the seed.** The seed becomes a commit on `c48046db2` that carries only
  the seed's additions.
- **Replay.** Every later Next commit, merges included, is re-created with the
  same author, committer, dates and message. Its tree is Next's tree plus any
  top-level entry the seed left out and that commit does not have. For
  example, `mobile/` is taken from `c48046db2`, while `native/` is Next's own
  after its import.
- **Determinism.** Rerunning the tool reproduces the same commit ids, so a
  re-sync only adds new commits.
- **Safety.** The tool refuses to rewind a branch that has diverged unless it
  is given `--force`, and it refuses a branch that is checked out in a
  worktree.

Re-sync after more work lands on Next main:

```sh
cd ~/projects/forkmesh/src
python3 tools/next_import.py --dry-run          # prints source, tip and how many commits are new
python3 tools/next_import.py                    # fast-forwards next-import
git -C ~/projects/forkmesh/worktrees/adopt-next merge next-import
```

After the folder rename, pass
`--next-repo ~/projects/forkmesh-backup/src`. To import a pinned commit,
pass `--next-ref <sha>`. The tests are in `deploy/tests/test_next_import.py`.

### Merge policy used for `adopt-next`

- **Next wins:** `app/`, `world/`, `www/`, `deploy/`, `app/host/`,
  `tools/next_*`, `tools/cutover_*`, and the Next docs.
- **Production wins:** `desktop/`, `native/`, `mobile/`, `marketplace/` and
  `codemagic.yaml`. Next's `CM_CLONE_UNSHALLOW` is kept on the native
  workflows.
- **`server/` keeps both sides:**
  - From Next: the agent backend and runner split and the injectable command
    runner.
  - From production: the CLI-version floor (`agentCLIFloor`) and image
    attachments. Attachment validation moved into `agentEnvelopeInvalid`, so
    it covers runner jobs too.
- **Migrations:** production's `0191_org_agent_attachments.sql` is dropped,
  because Next ported it as `0201`. Its down script is now
  `down/0201_org_agent_attachments.sql`. The schema fingerprint is Next's.
- **Hostnames:** Next's first rehost left `next.forkmesh.com` in files that
  the cutover inverse did not cover (`server/`, most of `tools/`,
  `tools/package/`, `www/docs/`, the Go module path, systemd packaging, and
  some App tests). Every such line whose production spelling exists
  unchanged on production main was restored to it. The references that
  remain are the fleet's own zone (`deploy/`, `app/host/`, `tools/next_*`,
  `tools/cutover_*`, `tools/rehost.py`), the core node origins `nN.next.forkmesh.com`
  in `www/worker.js`, the `wrangler.toml` route patterns, which are not
  deployed, the Next docs, and production's own `mobile/` project entry.
- **Integration fixes after the merge:**
  - Next moved the hosted-push pin into `_pin_hosted_state`, and production's
    code-push ping auto-merged into it without its `pusher` argument. It now
    takes `pusher` from `record_hosted_push_state`.
  - Production's attachments `schema.py` statements change the runtime schema
    fingerprint. The stamp-only migration `0205_org_agent_attachments_schema.sql`
    records the new fingerprint.
  - `deploy/tests/test_next_codemagic.py` checks only the native workflows
    that `tools/next_codemagic.py` starts, because `codemagic.yaml` also
    keeps production's Flutter and macOS workflows.
  - The attachments test reads the delete from `_apply_org_agent_job_result`,
    where Next's refactor moved it.
  - Tests whose sources keep Next's names (the `wrangler.toml` route patterns
    and the host request guard) keep Next's text.
- **Subsystem home pages:** both sides wrote `docs/<subsystem>/README.md`
  independently. Each page keeps Next's text and appends production's
  2026-09-24 refresh below it. `docs/README.md` merges both indexes.

## Part B: operator files

Status: **done 2026-09-24**; see "Done 2026-09-24" below. The inventory and plan that follow are the record of how it was prepared.

### Goal

ForkMesh Next is now production. Its operator files move out of `~/projects/forkmesh-next` and into the production project tree. After that, the old folder is renamed `~/projects/forkmesh-backup`.

| From | To |
|---|---|
| `~/projects/forkmesh-next/config/` | `~/projects/forkmesh/config/fleet/` |
| `~/projects/forkmesh-next/ssh/` | `~/projects/forkmesh/ssh/fleet/` |
| `~/projects/forkmesh-next/data/<live dirs>` | `~/projects/forkmesh/data/fleet/<same name>` |
| `~/projects/forkmesh-next/logs/` | `~/projects/forkmesh/logs/fleet/` |
| `~/projects/forkmesh-next/src` (code) | `~/projects/forkmesh/src` (Next code merged in: `tools/next_*.py`, `tools/cutover_*.py`, `deploy/`, `app/host/`) |
| everything else | stays in `~/projects/forkmesh-backup` (backup only) |

The folder rename happens only after the agents working in `~/projects/forkmesh-next/worktrees/` have finished. There are 27 git worktrees there today, and each one records absolute paths, so the rename breaks them. If any are still needed afterwards, run `git worktree repair`.

### 1. Inventory

#### config/ (0700, 76 KB)

Every tool finds these files through `next_common.config_dir()`, which returns `$FMNEXT_CONFIG_DIR` or, if that is unset, `~/projects/forkmesh-next/config`. Two tools ignore that setting: `next_rekey.config_dir()` is a copy of the function, and `cutover_data.py` builds the path directly in code.

| File | Mode | Read by (live) | Destination |
|---|---|---|---|
| `secrets.env` | 0600 | `next_fleet` (secrets, init-secrets), `next_vault` (sync, DATA_KEY fallback), `next_ids` (`FMNEXT_IDS_MASTER`), `next_backup:294`, `next_git_host:70`, `cutover_identity` | config/fleet |
| `DATA_KEY` | 0400 | `next_vault` unseal/sync (and the **fmnext-unseal timer**), `next_fleet:932`, `cutover_data:319` (hardcoded path), `next_rekey` | config/fleet |
| `ids-operator.key` | 0400 | `next_ids` release signing (`OPERATOR_KEY_FILE`, next_ids.py:65) | config/fleet |
| `fleet-state.json` | 0600 | `next_common.load_fleet:244`, `next_fleet:639/719/1252`, `deploy/core/render.py:104` (WireGuard pubkeys) | config/fleet |
| `fleet-instances.json` | 0600 | `next_fleet:283` | config/fleet |
| `known_hosts` | 0644 | `next_common.Ssh` (`UserKnownHostsFile`, next_common.py:188). Every fleet SSH call uses it, including backup, unseal, IDS and deploy | config/fleet |
| `vultr.env` | 0600 | `next_fleet:218` | config/fleet |
| `aws.env` | 0600 | `next_aws.load_credentials` | config/fleet |
| `cloudflare.env` | 0600 | `next_dns:78`, `cutover_dns:67`, `cutover_tls:87` | config/fleet |
| `codemagic-next.json` | 0600 | `next_codemagic:63` | config/fleet |
| `backup-age.pub` | 0644 | `next_backup` (recipient, next_backup.py:64). The backup timer refuses to run without it | config/fleet |
| `backup-age.key` | 0400 | `next_backup` verify/restore (identity, next_backup.py:65) | config/fleet, **or offline** (see Findings) |
| `next-admin.env` | 0600 | `cutover_data:165/664/766/817`, all hardcoded paths | config/fleet (admin credential record) |
| `turnstile.json` | 0600 | no code reads it; operator record | config/fleet |
| `mailtrap-webhook.json` | 0600 | no code reads it; its ID is mentioned in `deploy/fleet.toml:90` | config/fleet |
| `cutover-dns-state.json` | 0600 | `cutover_dns:25/152` (state for DNS cutover and rollback) | config/fleet while the cutover can still be rolled back, then backup only |
| `secrets.env.pre-cutover` | 0600 | `cutover_identity:36` (identity rollback) | backup only once the cutover is final |
| `DATA_KEY.next-pre-cutover` | 0400 | `cutover_identity:61` (rollback) | backup only once the cutover is final |
| `DATA_KEY.next-original` | 0400 | `next_rekey:671` default `--new-key` (a one-off rekey) | backup only |
| `deploy.lock`, `deploy-manifest.json`, `autodeploy/` | 0600 | `next_deploy` (lock, manifest), `next_autodeploy` (lock, kill switch, state); see [deploy/auto-deploy.md](deploy/auto-deploy.md) | config/fleet |
| *(missing)* `dns-health.json` | – | `next_dns:228` writes it when `health-loop` runs | created in config/fleet |
| *(missing)* `codemagic.env` | – | `next_codemagic:94` (optional) | – |

#### ssh/ (0700)

| File | Read by | Destination |
|---|---|---|
| `next_fleet_ed25519` (0600) + `.pub` | `next_common.ssh_key_path()` (`$FMNEXT_SSH_KEY` or `~/projects/forkmesh-next/ssh/next_fleet_ed25519`, next_common.py:180). Every fleet SSH call uses it | ssh/fleet |
| `known_hosts` (0644) | `next_git_host.ssh_known_hosts()`: `$FMNEXT_KNOWN_HOSTS` or `ssh_key_path().parent/known_hosts` (next_git_host.py:55-58). **It is not the same file as `config/known_hosts`**; the two differ in content | ssh/fleet |

#### data/

| Entry | Size | Read by | Destination |
|---|---|---|---|
| `backups/` (0700, `rqlite-*.sqlite3.gz.age`, `.work/`) | 9.4 MB | `next_backup.backups_dir()` (`$FMNEXT_BACKUPS_DIR` or hardcoded, next_backup.py:73-76), which the **fmnext-backup timer** writes to; retention prunes here | data/fleet/backups |
| `dist/` (`fmnext-<rev>.tar.gz` + `.sha256`, `forkmesh-mirror-node`) | 821 MB | `next_deploy.dist_dir()` (`$FMNEXT_DIST_DIR`, next_deploy.py:58-61); `next_fleet.install_daemon:845` (hardcoded, **ignores `FMNEXT_DIST_DIR`**) | data/fleet/dist: the current and previous release plus `forkmesh-mirror-node`. Older tarballs are backup only |
| `dl/rqlite-v10.3.6-linux-amd64.tar.gz` | 18 MB | `next_fleet.rqlite_tarball:613` (hardcoded), used by install and replace-core | data/fleet/dl |
| `dl/caddy_2.6.2_*` | 14 MB | no code reads it | backup only |
| `bin/rqlited`, `bin/rqlite` | 44 MB | `cutover_data._rqlite_bin:516-518` (`$FMNEXT_RQLITED` or hardcoded); tests `deploy/tests/test_rqlite_unit.py:33`, `test_next_backup.py:288` (`REPO.parent/data/bin`) | data/fleet/bin |
| `bin/caddy` | 41 MB | `deploy/tests/test_caddy.py:38` (`REPO.parent/data/bin/caddy`) | data/fleet/bin |
| `bin/caddy-fmnext`, `bin/rqbench` | 63 MB | no code reads them | backup only |
| `rekey/` (`STAMP`, `rekeyed-*.summary.json`) | 32 KB | `next_rekey.default_work_root:609` (hardcoded) | backup only. Create an empty 0700 `data/fleet/rekey` only if the default moves there |
| `cutover-tls/` (app/world `.crt`) | 20 KB | `cutover_tls:55-57` (`$FMNEXT_CUTOVER_TLS_DIR`); one-off cutover output | backup only |
| `cutover-20260924T151031Z/` | 34 MB | one-off cutover run (plaintext `next.sqlite3`, 0600) | backup only. Consider shredding it once the cutover is final |
| `cutover-rehearsal/` | 168 KB | one-off | backup only |
| `imports/` | empty | nothing | skip |
| *(missing)* `host-dev/` | – | `app/host/forkmesh_host/paths.py:20` local dev default (`$FORKMESH_HOST_DATA_DIR`) | – |

#### logs/ (384 KB)

About 95 files: `deploy-*.log`, `install-*.log`, `ids-*.log`, `vault-n*.log`, `guard-*`, `sec-*`, `perf*`, and two 0600 reports, `security-audit-{code,infra}-2026-09-24.md`. No tool reads them. The only exception is `next_rekey find-key` (next_rekey.py:1418-1419), which scans `<base>/data` and `<base>/logs` for key leaks. Destination: copy everything to logs/fleet (the size is negligible).

### 2. Destination collisions

| Target parent | Existing top-level names | Name added | Collision |
|---|---|---|---|
| `~/projects/forkmesh/config` (0775) | `fanout/`, `ForkMesh/`, `QtProject.conf`, `systemd/` | `fleet/` (0700) | none |
| `~/projects/forkmesh/ssh` (0700) | `known_hosts`, `known_hosts.old`, `user/`, `vultr_mirror_ed25519{,.pub}` | `fleet/` (0700) | none |
| `~/projects/forkmesh/data` | `actions agent-accounts agent-images agents announcer avatars chat_history deployments favicons identity local_control local_mesh mirrors network_log.d network_log.txt pings.json* private-replicas profile prompt pull-meta release saved-searches.json` | `fleet/` | none |
| `~/projects/forkmesh/logs` | 3 `deploy-app-*`/`desktop-rebuild-*` logs, `diagnostics/` | `fleet/` | none |

The production repo (`~/projects/forkmesh/src/tools`, `desktop/src`) never refers to `config/fleet`, `ssh/fleet`, `data/fleet` or `logs/fleet`.

### 3. Path references that must change

#### Box state (outside the repo)

| Where | Reference | Change |
|---|---|---|
| `~/.config/systemd/user/fmnext-backup.service` (installed **copy**, not a symlink; older header comment than the repo copy) | `WorkingDirectory=%h/projects/forkmesh-next/src`, `ExecStart=%h/projects/forkmesh-next/src/.venv/bin/python …/tools/next_backup.py run --execute` | point at `%h/projects/forkmesh/src` |
| `~/.config/systemd/user/fmnext-unseal.service` (copy, same as repo) | same pattern, `next_vault.py unseal --if-sealed --execute` | same |
| `fmnext-backup.timer` (enabled; daily 03:30 UTC ±15 min, Persistent) | no path | none |
| `fmnext-unseal.timer` (enabled; **every 1 min**) | no path | none |
| `fmnext-dns-health.service` | repo only, **not installed** on this box | fix in the repo only |
| `crontab -l` | empty | none |
| `~/.ssh/config` | no forkmesh-next entries (only prod mirror hosts) | none |
| `~/.bashrc`, `~/.profile`, `environment.d`, `systemctl --user show-environment`, the current env | no `FMNEXT_*` variables set anywhere; every tool uses its built-in defaults | none, unless the env-override route is chosen |
| `known_hosts` | Only through code: `config/known_hosts` via `Ssh` and `ssh/known_hosts` via `next_git_host`. No `UserKnownHostsFile` in `~/.ssh/config` points to Next | follows the code defaults |
| `forkmesh-backup-minimal.timer` (prod, hourly) | tars `~/projects/forkmesh/{config,ssh,…}` to `~/Backups/forkmesh` | **it will start capturing config/fleet and ssh/fleet**; see Findings |

The **venv is a blocker**. The units run `src/.venv/bin/python`. `~/projects/forkmesh/src/.venv` does not have `cryptography`, which `app/host/forkmesh_host/vault_crypto.py` and `cutover_tls.py` need; the Next venv has `cryptography 50.0.1`. The only third-party import in the operator tools is `cryptography`, and `tomllib` is in the standard library. Before the units switch over, install `cryptography` into the prod venv or create a dedicated venv.

#### Repo references (as imported from Next main; line numbers from Next `ea95f4c`)

**97 lines** in 31 files contain `projects/forkmesh-next` or `"projects" / "forkmesh-next"`: 27 in code, 11 in units and fleet config, 5 in tests, and 54 in docs. The code lines that decide behaviour:

| file:line | What | Env override today |
|---|---|---|
| tools/next_common.py:46 | `config_dir()` default | `FMNEXT_CONFIG_DIR` |
| tools/next_common.py:180 | `ssh_key_path()` default (also sets the `next_git_host` known_hosts) | `FMNEXT_SSH_KEY` |
| tools/next_rekey.py:602-605 | duplicate `config_dir()` | `FMNEXT_CONFIG_DIR` |
| tools/next_rekey.py:609 | `default_work_root()` data/rekey | `--work-root` flag only |
| tools/next_rekey.py:1418-1419 | `find-key` base (`data`, `logs`) | `--paths` |
| tools/next_backup.py:76 | `backups_dir()` | `FMNEXT_BACKUPS_DIR`, `--dir` |
| tools/next_deploy.py:61 | `dist_dir()` | `FMNEXT_DIST_DIR` |
| tools/next_fleet.py:613 | rqlite tarball in data/dl | **none** |
| tools/next_fleet.py:845 | mirror-node build output in data/dist | **none** (ignores `FMNEXT_DIST_DIR`) |
| tools/cutover_data.py:165, 664, 766, 817 | `config/next-admin.env` | **none** (ignores `FMNEXT_CONFIG_DIR`) |
| tools/cutover_data.py:319 | `config/DATA_KEY` | **none** |
| tools/cutover_data.py:518 | `data/bin/rqlited` | `FMNEXT_RQLITED` |
| tools/cutover_tls.py:57 | data/cutover-tls | `FMNEXT_CUTOVER_TLS_DIR` |
| deploy/core/render.py:104 | fleet-state.json fallback, from `[operator] config_dir` in fleet.toml | fleet.toml |
| app/host/forkmesh_host/paths.py:20 | data/host-dev (dev only) | `FORKMESH_HOST_DATA_DIR` |
| deploy/fleet.toml:30-33 (`[operator]` config_dir, ssh_key, backups_dir, dist_dir) | Only `config_dir` is read (by render.py). The other three keys are **dead**: nothing reads them | edit the file |
| deploy/fleet.toml:2, 152; deploy/fleet.example.toml:6, 33-36, 105 | comments and example | edit the file |
| deploy/operator/fmnext-backup.service:5, 16, 17 | unit | edit the file |
| deploy/operator/fmnext-unseal.service:6, 14, 15 | unit | edit the file |
| deploy/operator/fmnext-dns-health.service:16, 25, 26 | unit (`/usr/bin/python3`, which has cryptography 43.0.0) | edit the file |
| tools/next_fleet.py:33, 363 | docstring and generated fleet.toml header | edit the file |
| tools/next_common.py:7-8; tools/next_rekey.py:42, 1407 | docstrings and help text | edit the file |
| deploy/tests/test_rqlite_unit.py:3, 33; test_next_backup.py:288; test_caddy.py:3, 38; app/host/tests_rqlite/conftest.py:23 | `REPO.parent/data/bin/...`. After the merge this resolves to `~/projects/forkmesh/data/bin` (and to `worktrees/data/bin` inside a worktree) | point at data/fleet/bin, or use `RQLITED`/`PATH` |
| deploy/tests/conftest.py:5 | comment | edit the file |

Docs to rewrite (54 lines): REKEY.md 14, OPERATIONS.md 9, CUTOVER.md 9, PLAN.md 5, tools/README.md 3, VAULT.md 2, and one each in README.md, AGENTS.md:44, security/README.md, native/PROGRESS.md, native/DESKTOP_PARITY.md, deploy/README.md, deploy/core/edge_client.md and app/host/RQLITE_FINDINGS.md.

**Leave these unchanged.** They are not operator paths but remote-node paths and identity names: `/etc/forkmesh-next/*`, `/var/lib/forkmesh-next/git`, `/opt/forkmesh-next/*`, `/run/forkmesh-next/unseal.sock`, `RuntimeDirectory=forkmesh-next`, the Vultr tag `forkmesh-next` (next_fleet.py:61, server/runnerpool.go:31, fleet.toml `instance_tag`, the hard guard), the D1 `database_name = "forkmesh-next"` (app/wrangler.toml:277), the Worker names `forkmesh-next-{app,www,world}`, `forkmesh-next.git`, and the node env names `FMNEXT_IDS_*`, `FMNEXT_WG0_PRIVATE_KEY`, `FMNEXT_EOF`.

**IDS release flow** (`next_ids.py release`) uses `config_dir()` for `secrets.env` (`FMNEXT_IDS_MASTER`) and `ids-operator.key`, `ssh_key_path()` and `config/known_hosts` for SSH, and `next_vault` unseal for `DATA_KEY`. Once `next_common` moves, all of it moves with it; it has no hardcoded paths of its own.

### 4. Code-default change (lands on production main before the move)

- `tools/next_common.py`: add `project_dir()`, returning `$FMNEXT_PROJECT_DIR` or `~/projects/forkmesh`. Derive every default from it:
  - `config_dir()` = `$FMNEXT_CONFIG_DIR` or `project/config/fleet`
  - `ssh_key_path()` = `$FMNEXT_SSH_KEY` or `project/ssh/fleet/next_fleet_ed25519`
  - a new `data_dir()` = `project/data/fleet`
  - a new `logs_dir()` = `project/logs/fleet`
- Replace the hardcoded `Path.home()/…/forkmesh-next` constructions with these helpers:
  - `next_rekey` (drop its duplicate `config_dir`)
  - `next_backup`, `next_deploy`
  - `next_fleet:613/845` (also honour `FMNEXT_DIST_DIR`)
  - `cutover_data` (5 config paths plus rqlited)
  - `cutover_tls`
- Point `render.py:104` at the same default and drop the dead `[operator]` keys from `fleet.toml`.
- Give the tests one lookup for binaries: `RQLITED`/`CADDY`, then `data_dir()/bin`, then `PATH`.
- Keep every existing env override, so an emergency operator can always point back at `~/projects/forkmesh-backup`.
- Units: change `%h/projects/forkmesh-next/src` to `%h/projects/forkmesh/src`, with no `Environment=` lines (the code defaults are enough).

If the code change cannot land before the move, the stopgap is `Environment=FMNEXT_CONFIG_DIR=… FMNEXT_SSH_KEY=… FMNEXT_BACKUPS_DIR=…` in both units. That covers everything the timers run. It does **not** cover `next_fleet` install/daemon, `cutover_data` or `next_rekey`, which have no override.

### 5. Switch-over sequence

**Before you start:**
- `adopt-next` is merged into production main, and the section 4 defaults have landed.
- The prod venv has `cryptography`: `~/projects/forkmesh/src/.venv/bin/python -c 'import cryptography'`.
- No Next agents are running.
- No deploy or backup is in flight: `systemctl --user list-units 'fmnext*'` shows everything inactive.
- Pick a time away from 03:30 UTC.

```sh
N=~/projects/forkmesh-next; P=~/projects/forkmesh
# 1. stop timers (unseal gap: a node restarting now stays sealed until step 7)
systemctl --user stop fmnext-unseal.timer fmnext-backup.timer
systemctl --user is-active fmnext-unseal.service fmnext-backup.service   # expect inactive

# 2. copy, never move (preserves modes 0400/0600/0700, times)
install -d -m 0700 $P/config/fleet $P/ssh/fleet $P/data/fleet
install -d -m 0775 $P/logs/fleet
rsync -a $N/config/ $P/config/fleet/          # optionally exclude the backup-only files listed above
rsync -a $N/ssh/    $P/ssh/fleet/
rsync -a $N/data/backups $N/data/dl $N/data/bin $P/data/fleet/   # add --exclude for the backup-only files
install -d -m 0775 $P/data/fleet/dist
cp -a $N/data/dist/forkmesh-mirror-node $N/data/dist/fmnext-<current>.* $N/data/dist/fmnext-<previous>.* $P/data/fleet/dist/
rsync -a $N/logs/ $P/logs/fleet/
diff -rq $N/config $P/config/fleet; diff -rq $N/ssh $P/ssh/fleet    # expect no output
stat -c '%a %n' $P/config/fleet/* $P/ssh/fleet/*                      # modes match section 1

# 3. units
install -m 0644 $P/src/deploy/operator/fmnext-{backup,unseal}.{service,timer} ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user cat fmnext-backup.service fmnext-unseal.service | grep -n forkmesh-next   # expect none

# 4. verify from the new tree (all dry-run or read-only)
cd $P/src
.venv/bin/python -c 'import sys; sys.path.insert(0,"tools"); import next_common as c; print(c.config_dir(), c.ssh_key_path())'
.venv/bin/python tools/next_vault.py status
.venv/bin/python tools/next_vault.py unseal --if-sealed            # dry-run
.venv/bin/python tools/next_backup.py run                          # dry-run
.venv/bin/python tools/next_dns.py status
.venv/bin/python tools/next_ids.py status
.venv/bin/python tools/next_fleet.py list                          # Vultr read
.venv/bin/python tools/next_git_host.py setup                      # dry-run, uses ssh/fleet/known_hosts
.venv/bin/python tools/next_codemagic.py status
FMNEXT_CONFIG_DIR=/nonexistent .venv/bin/python tools/next_vault.py status   # must FAIL: proves nothing still reads $N silently

# 5. one real run through systemd
systemctl --user start fmnext-unseal.service && journalctl --user -u fmnext-unseal -n 20 --no-pager
systemctl --user start fmnext-backup.service && ls -l $P/data/fleet/backups | tail -2

# 6. re-enable
systemctl --user start fmnext-unseal.timer fmnext-backup.timer
systemctl --user list-timers --all --no-pager | grep fmnext

# 7. after agents finish: rename (makes any missed forkmesh-next reference fail loudly)
mv ~/projects/forkmesh-next ~/projects/forkmesh-backup
grep -rn 'forkmesh-next/' ~/.config/systemd/user/ ; systemctl --user list-timers --no-pager | grep fmnext
```

Watch the next 03:30 UTC backup, and one `next_deploy.py build` into `data/fleet/dist`, before you consider the migration done.

#### Rollback

All operator state was copied, not moved, so rolling back only means pointing things at the original files again.

1. `systemctl --user stop fmnext-unseal.timer fmnext-backup.timer`
2. If step 7 has already run, undo the rename: `mv ~/projects/forkmesh-backup ~/projects/forkmesh-next`.
3. Reinstall the old unit copies from `~/projects/forkmesh-next/src/deploy/operator/` (they still reference `forkmesh-next`), or add `Environment=FMNEXT_CONFIG_DIR=%h/projects/forkmesh-next/config FMNEXT_SSH_KEY=… FMNEXT_BACKUPS_DIR=…` to the new units. Then `daemon-reload` and start the timers.
4. State written in the new location after the switch has to be merged back by hand: new backups in `data/fleet/backups`, `fleet-state.json`/`fleet-instances.json` changes from any `next_fleet` run, `dns-health.json`, and new dist tarballs. Compare with `diff -rq` before you delete either copy.
5. Leave `config/fleet` and `ssh/fleet` where they are until you decide which copy is authoritative. Delete a copy only by explicit file name (see safe-file-cleanup).

### Done 2026-09-24 (23:02 UTC)

- **Code** (main 8fcbfb39b): `tools/forkmesh_paths.py` is the one source. `Layout.fleet_config`, `fleet_ssh`, `fleet_ssh_key`, `fleet_data` and `fleet_logs` are `~/projects/forkmesh/<root>/fleet` once that folder exists, else the old `~/projects/forkmesh-next/<root>`. `next_common.project_dir/config_dir/ssh_key_path/data_dir/logs_dir` derive from it (`FMNEXT_CONFIG_DIR`, `FMNEXT_SSH_KEY`, `FMNEXT_DATA_DIR`, `FMNEXT_LOGS_DIR`, `FMNEXT_BACKUPS_DIR`, `FMNEXT_DIST_DIR` still override). No tool builds a `forkmesh-next` path any more; `next_fleet` rqlite/caddy/mirror-node paths and `cutover_data` follow `config_dir`/`data_dir`; the dead `[operator]` keys are gone; `render.py` reads `fleet-state.json` from the same config dir. Tests find `rqlited`/`caddy` through `Layout.fleet_binary` (`$RQLITED`/`$CADDY`, then `data/fleet/bin`, then `PATH`).
- **Venv**: `~/projects/forkmesh/src/.venv` has `cryptography 50.0.1`. All three units (`fmnext-backup`, `fmnext-unseal`, `fmnext-dns-health`) run `%h/projects/forkmesh/src/.venv/bin/python` from `%h/projects/forkmesh/src`.
- **Copied, verified, swapped**: each of `ssh/fleet`, `data/fleet`, `logs/fleet`, `config/fleet` was staged in a `.fleet-staging-<ts>` sibling, checked file by file (sha256, mode, owner, symlink target) and then renamed into place, so the desktop and the tools never saw a half-filled folder. `ssh/fleet` also holds `next_logread_ed25519` (+ `.pub`), the desktop's node-log reader key.
  - config/fleet: everything except `backup-age.key` (the private age key goes offline with the operator), `secrets.env.pre-cutover`, `DATA_KEY.next-pre-cutover`, `DATA_KEY.next-original` and `dns-health.json.lock`.
  - data/fleet: `backups/`, `dl/` (rqlite v10.3.6 and caddy 2.11.4 with its checksum files), `bin/rqlited` and `bin/rqlite`, `dist/` (releases 0a06bc133cd2 and 26a88c4476a8 plus `forkmesh-mirror-node`). The old `bin/caddy` is Debian's 2.6.2 and fails `test_caddy`, so it stayed behind; point `CADDY` at a 2.11.4 binary to run that test.
  - Backup only (in `~/projects/forkmesh-backup/data`): `cutover-20260924T151031Z/`, `cutover-rehearsal/`, `cutover-tls/`, `rekey/`, `imports/`, `mirror-final-backups/`, `old-stack-export/`, older dist tarballs, `dl/caddy_2.6.2*`, `bin/caddy`, `bin/caddy-fmnext`, `bin/rqbench`.
- **Hourly backup** (`~/projects/forkmesh/bin/forkmesh-backup-minimal.sh`, box only; the previous version is `forkmesh-backup-minimal.sh.pre-fleet`): the plain archive excludes `config/fleet` and `ssh/fleet` and refuses to keep an archive that contains them. Those two folders go into `~/Backups/forkmesh/forkmesh-fleet-<stamp>.tar.gz.age`, encrypted to `config/fleet/backup-age.pub`; the tar is piped straight into `age`, so no plaintext copy is written. A new fleet archive is written only when the file list, sizes, times or modes change. Without `age` or the recipient it writes no fleet archive and the unit fails.
- **Renamed** `~/projects/forkmesh-next` to `~/projects/forkmesh-backup`, then `git worktree repair` for all 35 worktrees of the old Next repo (the production repo had none there).
- **Verified**: IDS normal on n1-n3 and r1; `next_deploy.py all` dry run builds into `data/fleet/dist`; the DNS health loop runs from the new tree and writes `config/fleet/dns-health.json`; the unseal timer and one real `fmnext-backup` run are clean; the source publisher pass is OK; an SSH login to n1 with `ssh/fleet/next_fleet_ed25519` and `config/fleet/known_hosts` works. The hourly run produced a plain archive with no fleet members plus an age v1 archive with one X25519 recipient stanza. Final proof: `next_fleet.py install nN --steps files,start --execute` on n1, n2, n3 one at a time, each drained first and undrained after an IDS-normal check; this also applied the IDS `inet_diag`/`tcp_diag` module expectation.

#### Rollback after the move

1. `systemctl --user stop fmnext-unseal.timer fmnext-backup.timer fmnext-dns-health.service`.
2. `mv ~/projects/forkmesh-backup ~/projects/forkmesh-next`, then `git -C ~/projects/forkmesh-next/src worktree repair ~/projects/forkmesh-next/worktrees/*`.
3. Point the tools back without deleting anything: `Environment=FMNEXT_CONFIG_DIR=%h/projects/forkmesh-next/config FMNEXT_SSH_KEY=%h/projects/forkmesh-next/ssh/next_fleet_ed25519 FMNEXT_DATA_DIR=%h/projects/forkmesh-next/data FMNEXT_LOGS_DIR=%h/projects/forkmesh-next/logs` in the three units (the desktop and `forkmesh_paths` follow `config/fleet` and `ssh/fleet` while they exist, so moving those aside by rename is the only way to switch the desktop back). `daemon-reload`, then start the timers and the DNS loop.
4. Merge back by hand whatever was written in the new tree after the switch: `data/fleet/backups`, `config/fleet/fleet-state.json` / `fleet-instances.json` / `dns-health.json`, new `data/fleet/dist` tarballs, `logs/fleet`. Compare with `diff -rq` first.
5. `cp ~/projects/forkmesh/bin/forkmesh-backup-minimal.sh.pre-fleet ~/projects/forkmesh/bin/forkmesh-backup-minimal.sh` restores the old backup script. It tars `config/` and `ssh/` whole, so it would include `config/fleet` in plaintext while that folder exists.

### Findings and open decisions

- **The prod hourly backup will copy fleet secrets in plaintext.** `forkmesh-backup-minimal` tars all of `~/projects/forkmesh/config` and `ssh` into `~/Backups/forkmesh/*.tar.gz`. After the move, those tarballs would contain `DATA_KEY`, `secrets.env`, `ids-operator.key`, `backup-age.key` and the fleet SSH key. That conflicts with the security pillar (no plaintext copies). Decide whether to exclude `config/fleet` and `ssh/fleet` (or age-encrypt them) **before** step 2.
- **`backup-age.key` sits next to the backups it decrypts.** `fmnext-backup.service` says the private key should be moved offline after `keygen`. The move is a chance to do that.
- **The prod venv has no `cryptography`.** Unless that is fixed first, the unseal timer fails after the unit switch.
- **The unseal timer runs every minute.** While it is stopped, a node that restarts stays sealed and returns 503, so keep the window short.
- **There are two different `known_hosts` files.** `Ssh` uses `config/known_hosts`; `next_git_host` uses `ssh/known_hosts`. Both are carried. Consider unifying them on `ssh/fleet/known_hosts`.
- **Three `fleet.toml [operator]` keys are dead**, and `next_fleet` ignores `FMNEXT_DIST_DIR`.
- **`cutover_data` hardcodes `config/` five times** and ignores `FMNEXT_CONFIG_DIR`.
- The data folder holds a 34 MB plaintext `next.sqlite3` (0600) from the cutover run, plus pre-cutover key copies. They are backup only, and are candidates for deletion by name once the cutover is final.
- Unrelated: `~/.ssh/config` hosts mirror17-21 point at `~/projects/forkmesh/ssh/fanout/*_ed25519`, and that directory does not exist.

## Part C: growing the fleet with a new core

Added 2026-09-30. Three cores (`n1`–`n3`, rqlite voters) and one runner were
the adopted fleet. A fourth core is added with one command, or with
**Network → Hosts → Add core node** on an operator desktop (an admin account
and a `config/fleet/fleet-instances.json` on this machine):

```sh
python3 tools/next_fleet.py add-core                 # full plan, read-only
python3 tools/next_fleet.py add-core --execute       # create and join n4
python3 tools/next_fleet.py add-core-status          # where the last run is
```

The full order of operations, timings, parallel branches and the rqlite and
DNS rules are in [deploy/README.md](deploy/README.md#adding-a-core-node). In
short: plan → hold out of DNS → VM (the local prefetch runs alongside) →
fleet file and the node's own name → keys → peers re-rendered one at a
time, while the new core installs → rqlite join (a non-voter while there are
3 voters) → mirror daemon and repository sync, while the release uploads →
activate and unseal → every repository present → IDS baseline → health →
DNS → registration check → commit on `fleet/add-core-nN`. It takes about
14 minutes.

### Operator files it writes

| Path | What |
|---|---|
| `config/fleet/add-core/nN.json` | The run's state: each step's status and duration, instance id, IPs, release, rqlite role, finished peers, branch and commit. A re-run resumes from it. |
| `config/fleet/fleet-instances.json` | The new `fmnext-core-N` entry (as `create-core` writes it). |
| `config/fleet/fleet-state.json` | The node's WireGuard and IDS public keys; `rqlite_nonvoters` when it is a non-voter. |
| `config/fleet/dns-health.json` | The node, `held` and `joining` until the DNS step releases it. |
| `logs/fleet/ops/<stamp>-next_fleet-add-core.log` | The run's own log (every `--execute` run). |
| `logs/fleet/add-core/<stamp>.log`, `current.json` | The desktop's copy of the output. The desktop runs the command as the user unit `fmnext-add-core-<stamp>` so that it outlives the dialog and the desktop itself, and it reattaches through `current.json`. |
| `data/fleet/dist/fmnext-<rev>.tar.gz` | The served release, copied from a core when the desktop no longer has it. |
| `deploy/fleet.toml` in the checkout it ran from, and the branch `fleet/add-core-nN` | The new `[[nodes]]` block. Merge it with `git merge --autostash --no-ff fleet/add-core-nN`: the working tree already holds the same file, so a plain merge refuses. |

All of these are under the never-erase folders.

### Recovery

- **A step failed.** Read the failure and the notes (the dialog shows both,
  and so does the end of the ops log). Fix the cause, then press Retry or
  re-run `add-core --execute`. The run resumes at the failed step and skips
  peers that were already updated. If the failure came after the DNS step,
  the node was drained, and health and DNS run again before it goes back.
- **A peer was left drained** (the peer roll stops with the peer out of
  DNS when its health gate fails). Check it, then
  `next_dns.py undrain nX --execute`; Retry does the rest.
- **Give up on the new core.** It is out of DNS and held. Remove it from
  the rqlite cluster (from a peer, `rqlite remove nN`), then
  `next_fleet.py destroy nN --yes --execute`, and delete its `[[nodes]]`
  block and branch by hand. Re-render the peers
  (`next_fleet.py roll --steps files,start --execute`) so their WireGuard and
  mesh peer lists drop it. Its state file stays as a record.
- **The desktop was closed or restarted mid-run.** The unit keeps running.
  Reopening the dialog reattaches to it; if the unit already finished, the
  result is read back from its log.
