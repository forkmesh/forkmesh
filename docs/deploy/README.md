# Deploy: the core fleet

`deploy/` turns one fleet description, `deploy/fleet.toml`, into every file a
fleet node needs. The operator tools in `tools/next_*.py`, run from the
operator desktop, create instances, install nodes, push secrets and roll out
app releases. There are two node kinds: three **core** nodes (trusted: Caddy,
the app on CPython, rqlite, the Go daemon and Git gateway) and **runners**
(untrusted, outbound-only agent compute). The step-by-step runbook is
[OPERATIONS.md](../OPERATIONS.md); this page is the map.

## Code map

| Path | Responsibility |
| --- | --- |
| `deploy/fleet.toml` | The live fleet: nodes `n1` to `n3` (`fmnext-core-1..3`), runner `r1` (`fmnext-run-1`), and the `[paths]`, `[operator]`, `[wireguard]`, `[rqlite]`, `[app]`, `[dns]`, `[ids]` sections. Holds no secrets and no public addresses (see Node addresses). |
| `deploy/fleet.example.toml` | Annotated template for a new fleet. |
| `deploy/core/render.py` | Renders a node's files into a directory tree mirroring the target filesystem, plus `MANIFEST.json` (mode of each file, credentials each unit loads). Never reads secrets. |
| `deploy/core/caddy_routes.py` | Caddy route snippets for the app and world sites; shipped with each app release. |
| `deploy/core/request_rules.py` | `fmnext-request-rules`: renders the app's `request_block_rules` export into the Caddy include `request-rules.caddy` and reloads Caddy when it changes ([../host/README.md](../host/README.md), "Request rules in Caddy"). |
| `deploy/core/certsync.py` | Leaderless certificate sharing between cores (`fmnext-certsync serve` and `sync`). |
| `deploy/core/ids.py` | The host intrusion detector `fmnext-ids` ([IDS.md](../IDS.md)). |
| `deploy/core/edge_client.md` | Wire contract for `x-forkmesh-edge-client`, the visitor identity the homepage Worker signs. |
| `deploy/core/templates/` | Unit files and configs rendered per node: `forkmesh-app.service`, `rqlite.service`, `forkmesh-mirror-node.service`, `fmnext-certsync.service`, `fmnext-cert-sync.{service,timer}`, `fmnext-request-rules.{path,service}`, `fmnext-ids.service`, `caddy-dropin.conf`, `app.env`, `hosts.fragment`, `journald.conf`, `sysctl.conf`, `zram-generator.conf`, `swapfile.swap`, `earlyoom`, and the node-side helpers `fmnext-unseal` and `fmnext-app-ops`. |
| `deploy/runner/templates/runner.md` | The README placed on each runner. |
| `deploy/operator/` | User units for the operator desktop: `fmnext-backup.{service,timer}` (nightly backup), `fmnext-unseal.{service,timer}` (unseal sealed cores every minute), `fmnext-dns-health.service`. |
| `deploy/tests/` | Tests for the renderer, Caddy, certsync, IDS, cutover and every `tools/next_*.py`. |
| `tools/next_common.py` | Shared SSH, fleet loading and dry-run plumbing. |
| `tools/next_fleet.py` | `plans`, `list`, `create`, `create-core`, `create-runner`, `fleet-toml`, `init-secrets`, `install`, `secrets`, `destroy`, `replace-core`, `add-core`, `add-core-options`, `add-core-status`, `remove-core`, `remove-core-plan`, `remove-core-status`. `--provider vultr\|aws` on `plans`, `list`, `create*` and `add-core*` (see Cloud providers). |
| `tools/next_cloud.py` | The provider interface (`Provider`: `guard`, `fleet_instances`, `find_by_label`, `instance`, `create_instance`, `delete_instance`) and registry: the provider recorded per label in `fleet-instances.json`, default `vultr`. |
| `tools/next_aws.py` | AWS EC2 provider: standard-library SigV4, EC2/SSM/STS calls over `urllib`, the AWS guards, key pair, security group and instance creation. |
| `tools/next_add_core.py` | The resumable `add-core` run: step engine, state file, parallel branches, containment, and the fleet backend (see Adding a core node). |
| `tools/next_remove_core.py` | The resumable `remove-core` run: quorum preflight, steps, SSH identity check, provider dispatch for the destroy step (see Removing a core node). |
| `tools/next_deploy.py` | `build`, `upload`, `migrate`, `restart`, `all`, `rollback`. Takes the deploy lock with `--execute` and records full-fleet releases in the deploy manifest. |
| `tools/next_autodeploy.py`, `.forkmesh/deploy-nodes.yml` | Auto-deploy of `main` to the cores through ForkMesh Actions ([auto-deploy.md](auto-deploy.md)). |
| `deploy/core/self_deploy.py` | `fmnext-self-deploy` on each core: deploys phone-approved merges, app releases and the Go daemon, without the desktop ([self-deploy.md](self-deploy.md)). |
| `tools/next_vault.py` | Secrets vault, operator side ([VAULT.md](../VAULT.md)). |
| `tools/next_ids.py` | IDS, operator side ([IDS.md](../IDS.md)). |
| `tools/next_dns.py` | Cloudflare DNS for the production `app.forkmesh.com` and `world.forkmesh.com` records and names under `next.forkmesh.com` (each core's own `nN` name, and `app.next`/`world.next`, which redirect to production). |
| `tools/next_backup.py` | Nightly rqlite backup pulled to the operator box, age-encrypted. |
| `tools/next_rekey.py` | Offline re-key of a database copy ([REKEY.md](../REKEY.md)). |
| `tools/next_git_host.py`, `tools/next_codemagic.py` | Private Git over HTTPS on the cores; Codemagic builds of the native iOS app. |

## Architecture

- **Render, then push.** `render.py` produces files only. `next_fleet.py
  install <node> --steps ...` copies them and runs each step over SSH. Core
  steps: `base`, `wireguard`, `rqlite`, `files`, `secrets`, `start` (plus
  `daemon` and `ids` on request). Runner steps: `base`, `files`, `start`.
- **Secrets never rendered.** Units load systemd encrypted credentials by name
  (`LoadCredentialEncrypted=`). `next_fleet.py secrets` pushes them. With
  `[app] vault = true` the app unit gets only bootstrap credentials and starts
  sealed; the rest live sealed in the database ([VAULT.md](../VAULT.md)).
- **Releases.** `next_deploy.py build` makes a tarball of the built app
  (`app/host`, `app/src_build`, `app/dist`, `app/wrangler.toml`), the built
  World, a `.venv` from a bundled wheelhouse, the Caddy snippets, `BUILD_REV`
  and `release.env`. On a node it lands in
  `/opt/forkmesh-next/app/releases/<rev>`; `current` is a symlink and
  `previous` keeps the last one. Static assets are precompressed at build.
  On the operator box each build prunes `data/fleet/dist` to the
  `KEEP_RELEASES` newest `fmnext-<rev>.tar.gz` (plus the one just built):
  nothing pruned them before 2026-10-07, and 796 files (10 GB, ~30 MB per
  deploy) helped fill the disk until deploys failed with ENOSPC. An older
  release can be rebuilt, or `next_add_core` copies it from a core.
- **Rolling restart gate.** Each node restart waits for
  `127.0.0.1:8080/readyz` and `/api/version` to report the new `BUILD_REV`; a
  failing node is relinked to its previous release and the rollout stops.
- **Migrations** run once, through any healthy rqlite node.
- **Rollout order** rotates per release: `rollout_order(fleet, rev=...)`
  sorts the cores by sha256 of `rev:name`, so the first core (the canary of
  `--parallel`) changes from release to release (see "No fixed main node").
- **No leader.** Cron, Durable Object placement, DNS health votes and the
  shared certificate are leaderless (see [docs/host/README.md](../host/README.md)
  and OPERATIONS.md section 4).
- **Runners** hold no core secret and are not WireGuard peers. The daemon runs
  as root and starts each job as `forkmesh-job` in its own systemd scope
  ([RUNNERS.md](../RUNNERS.md)).

## Deploy speed

What `next_deploy.py all --execute` does to keep a rollout short (2026-09-26):

- **One SSH connection per node.** `next_common.Ssh` sets
  `ControlMaster=auto`, `ControlPersist=120` and a `ControlPath` in a private
  `mkdtemp` directory under `$XDG_RUNTIME_DIR` (or the system temp directory).
  A rollout makes about ten SSH or scp calls per node (upload, IDS expectation,
  activation, state check, rqlite status), and each call used to do a fresh
  TCP and key-exchange handshake. `Ssh.close()`, registered with `atexit`,
  stops the masters and removes the directory. Port forwards (`Ssh.tunnel`)
  never use the master, so ending a tunnel cannot take other sessions with it.
  `FMNEXT_SSH_MULTIPLEX=0` turns it off.
- **Uploads in parallel, migration alongside.** `upload` sends the release to
  every serving node at once (`next_common.parallel_steps`). One failed node
  fails the step after the others finish. `all` runs the migration during the
  uploads. This is safe because the rollout is already rolling: old code
  always runs for a while against the migrated schema. The restart stays one
  node at a time, behind the gate.
- **Venv reuse on the node.** `upload_script` copies the current release's
  `.venv` when `app/host/requirements.txt` is byte-identical and the
  wheelhouse holds the same wheel file names. It rewrites the console-script
  shebangs to the new path, then still runs the offline
  `pip install --no-index`, which confirms every requirement from the new
  wheelhouse. Otherwise it builds a fresh venv as before. On a workstation
  this took the node step from 4.2 s to 0.9 s; a 1-vCPU core gains more.
- **Wheelhouse cache.** The build downloads wheels once per
  `requirements.txt`, Python version and platform set, into
  `$FMNEXT_DIST_DIR/wheelhouse/<key>`, and copies them into each release.
- **Batched schema replay.** After a migration, `rqlite_migrate.py` replays
  the app's own DDL (about 690 statements). It used to send one HTTP request
  per statement through the SSH tunnel, which took 32 s on 2026-09-24. It now
  sends batches of 100 without a transaction. rqlite runs every statement and
  returns one result each, so ALTERs that fail can still be tolerated one by
  one.

QUIC was considered and not used. Deploys go over SSH and scp, and repository
sync goes over Git smart HTTP; neither can run over QUIC. Git has no HTTP/3
mode, and adding `quic-go` to the daemon would not change its `git fetch`.
The time went to handshakes, serial per-node steps and per-statement or
per-ref round trips, which QUIC would not remove.

## Adding a core node

`python3 tools/next_fleet.py add-core [--region R] [--plan P] [--snapshot ID] --execute`
(code in `tools/next_add_core.py`) creates a new core and joins it to the
fleet in one run. The desktop's **Add core node** button (Network → Hosts,
next to the fleet table) runs the same command
([../desktop/networking.md](../desktop/networking.md#add-core-node)). Without
`--execute` it prints the full plan, read-only, against the real fleet files.
`add-core-options --json` gives the dialog its defaults, plan prices and
estimate. `add-core-status --json` reports the last run.

### Order of operations

The estimates are wall-clock times on the 1 vCPU / 1 GB cores. `||` marks
branches that run in parallel.

| # | step | about | what it does |
|---|------|-------|--------------|
| 1 | plan | 2 s | Picks the next number above every core in `fleet.toml`, `fleet-instances.json`, Vultr and unfinished runs (so `n4` today). Takes the region and plan the existing cores use, decides the rqlite role, and reads the served release. `--execute` checks the live `/nodes` voters against `fleet.toml` and requires every core to run the same release. |
| 2 | hold | 1 s | Writes `nN` into `dns-health.json` as `held` and `joining` before the fleet file names it, so the DNS health loop can never add it early. |
| 3 | vm | 120 s | Creates the Vultr instance (label `fmnext-core-N`, tag `forkmesh-next`, IPv6 on at creation so no reboot is needed later, backups off), or adopts it if it exists. Waits for it to be active and to answer SSH. **\|\|** Locally: the release tarball at the served rev (from `data/fleet/dist`, or copied from a core's `releases/<rev>.tar.gz` and checked against its `.sha256`), the wheelhouse, a warm `go build` of the mirror daemon, and the rqlite and Caddy tarballs checked against `fleet.toml`. |
| 4 | fleet | 10 s | Inserts the node's `[[nodes]]` block into `deploy/fleet.toml` (`core_block` + `upsert_core_block`; the rest of the file is left byte for byte) and records a non-voter in `fleet-state.json` `rqlite_nonvoters`. Creates the node's own name `nN.next.forkmesh.com` A/AAAA, which its certificate needs; the served names are not touched. Sets the mail PTR when core mail is set up. |
| 5 | keys | 40 s | `apt install wireguard-tools python3-cryptography`, then `install --steps wireguard,ids`. The private keys stay on the node; the public keys go into `fleet-state.json` and the fleet file. |
| 6 | mesh | 300 s | Existing cores, one at a time, through `roll --steps files,start`: drain, re-render (WireGuard peer, mesh peers, IDS keys, rqlite node map), health gate, undrain. **\|\|** New core: `install --steps base,rqlite,files,caddy,secrets`, then mail and the SPF record when mail is set up. Each finished peer is recorded, so a retry skips it. |
| 7 | join | 45 s | `install --steps start`: wg0 comes up and rqlite joins over the mesh. Waits for rqlite `/readyz` on the node and for a reachable entry with the chosen role in a peer's `/nodes`, while the voters from step 1 stay healthy. |
| 8 | services | 150 s | `install --steps daemon,files,start` (mirror daemon, gateway, the daemon tools), then `POST /v1/control/sync`: the daemon fetches every served repository from the peers (the node is not in DNS yet, so the served app name, `app.forkmesh.com`, resolves to the peers). **\|\|** `next_deploy upload` of the release, with the venv built from the cached wheelhouse. |
| 9 | activate | 60 s | `next_deploy restart` for this node only: IDS expectation, `current` switch, DATA_KEY on SSH stdin, gated on `/readyz`, `/api/version` = the rev and Caddy. Then `next_vault unseal --if-sealed`. No migration runs: the release and the database are the ones the fleet already serves. |
| 10 | repos | 60 s | Compares the daemon's `/v1/status` repositories with a peer's until every one is present with `lastSyncOk`, nudging a sync every 60 s (at most 20 min). |
| 11 | ids | 20 s | `install --steps logread`, then `next_ids baseline`. |
| 12 | health | 20 s | On the node: `/readyz` 200, `forkmesh-app` and `forkmesh-mirror-node` active, IDS normal. From the desktop with `--resolve`: the app's `/readyz`, `/api/version` = the rev, `nN.next/readyz`, `/dashboard`, `/login` and the World page (`next_autodeploy.verify_once`). |
| 13 | dns | 15 s | `next_dns undrain`: 3 probes over IPv4 and IPv6, then the node is added to `app.forkmesh.com`, `world.forkmesh.com`, `app.next` and `world.next` (the last two redirect to production). |
| 14 | register | 2 s | Checks that the DNS health loop has the node released (in DNS, not held). Backups, the desktop's fleet table and status read the fleet file and `fleet-instances.json`, so nothing else needs registering. |
| 15 | commit | 2 s | Commits the `deploy/fleet.toml` change on `fleet/add-core-nN`, based on `main`, with plumbing (a temporary index, `commit-tree`, `update-ref`): HEAD, the index and the working tree are not touched. It prints `git -C <repo> merge --autostash --no-ff fleet/add-core-nN`. Nothing is pushed. |

The total is about 14 minutes, and the peer roll takes about 5 of them. The
whole run holds `deploy.lock`, so autodeploy and `roll` wait for it.

### rqlite role

The rule is to keep an odd number of voters (`choose_voter`). With 3 voters
today, a fourth voter would still survive only one failure, and every write
would need 3 acknowledgements instead of 2. So `n4` joins as a
**non-voter**: a read replica that follows the log, gets
`rqlite_voter = false` in its `[[nodes]]` block, and renders
`-raft-non-voter -join <voters>`. A fleet with an even number of voters (after
a loss, say) gets a voter. Going to 5 voters is a separate step: remove the
non-voter (`rqlite remove nN`) and rejoin it without `rqlite_voter = false`
alongside a new voter. The tool never does that on its own. The renderer's
`-bootstrap-expect` and `-join` lists count only voters, so adding a
non-voter leaves every existing core's `rqlite.service` byte for byte the
same.

### DNS rules

- `next_dns.load_state` holds a core it has not seen before out of DNS,
  whenever the state already has a node in DNS. A core that shows up in
  `fleet.toml` by any route waits for an explicit `undrain`, which probes it
  first, and the health loop never adds it on its own. `next_dns.py ensure`
  skips held cores, and only the node's own `nN.next` name is created early.
- The DNS step comes after health, and the engine re-runs `health` and
  `dns` whenever a later step fails, because the failure handler drains the
  node.

### Idempotency, resume and recovery

- State: `config/fleet/add-core/nN.json` records each step's status and
  duration, the instance id, IPs, rev, role, finished peers, branch and
  commit. Re-running `add-core --execute` (or pressing Retry) resumes the one
  unfinished run from its failed step. Starting it again with a different
  `--region`/`--plan` is refused. A finished node is refused as "already
  added".
- Every step is safe to repeat: `create` adopts the labelled instance, the
  install steps and the renders are idempotent, the fleet block is upserted,
  and the branch commit is reused when its content already matches.
- On a failure the run marks the step failed, drains the new node if it had
  reached DNS (otherwise it stays held), and prints what is out of DNS,
  including a peer that `roll` left drained, plus the VM id. The VM is kept
  for the retry; `next_fleet.py destroy nN --yes --execute` removes it.
  Nothing on the existing cores is deleted: they only get re-rendered config.
- Snapshots: cloning a live core's disk would copy its WireGuard, IDS and
  node keys, its encrypted credentials, its TLS material and its rqlite Raft
  state. `create_snapshot` refuses anything but `fmnext-snap-N` builders for
  that reason. `--snapshot ID` accepts the keyless autoscale boot image
  (`next_autoscale.py snapshot build`), which already has the packages,
  rqlite, Caddy and the daemon, so `base` becomes mostly no-ops.
- Merging: the run writes the same `deploy/fleet.toml` into the checkout it
  runs from, so the DNS health loop (which reads the working tree) sees the
  node before the merge. A plain `git merge` refuses to overwrite that
  identical local copy, and `--autostash` merges cleanly
  (`deploy/tests/test_next_add_core.py`). Autodeploy, `roll` and the
  autodeploy verify read `main`, so merge soon after the run: until then a
  deploy skips the new core.

Tests: `python3 -m pytest deploy/tests/test_next_add_core.py` (fakes for
Vultr, SSH, Cloudflare and rqlite; no network).

## Removing a core node

`python3 tools/next_fleet.py remove-core nN [--destroy-instance] --execute`
(code in `tools/next_remove_core.py`) takes a core out of the fleet in one
run, so the fleet can shrink as easily as it grows. The desktop's
**Remove…** button on each core row of the fleet table runs the same command
([../desktop/networking.md](../desktop/networking.md#remove-core-node)).
Without `--execute` it prints the plan, including the live quorum check, and
exits 2 when the removal would be refused. `remove-core-plan nN --json` gives
the dialog the plan, the quorum impact and whether it is allowed;
`remove-core-status --json` reports the last run.

### Order of operations

| # | step | about | what it does |
|---|------|-------|--------------|
| 1 | preflight | 5 s | `nN` must be a core in `fleet.toml`. Reads `/nodes` from a peer and refuses when fewer than 3 cores or 3 healthy rqlite voters would remain, or when the healthy voters left would not be a majority of the voters left. A reachable non-voter core that the next step promotes counts as a voter here. Refuses while an add-core run is unfinished. Checks the node's identity over SSH (below). |
| 2 | promote | 120 s | Only when `nN` is a voter and the voters left would be fewer than 3 or even, and a reachable non-voter core exists (`promotion_candidate`, lowest number first). That core is drained, removed from rqlite as a non-voter, re-rendered as a voter (`promote_core_block` drops `rqlite_voter = false` from its block; `fleet-state.json` drops it from `rqlite_nonvoters`; its `rqlite.service` gets `-join <voters>`), its rqlite restarted, and the step waits until `/nodes` lists it as a reachable voter, then gates on health and undrains it. Otherwise it does nothing. |
| 3 | leader | 15 s | Only if `nN` leads rqlite: `POST /leader` on it (rqlite steps down and hands leadership over), then waits until a peer reports another leader. |
| 4 | drain | 75 s | `next_dns` drain: out of `app`/`world` (production and `.next`), held so the health loop never re-adds it; refused if it is the last node in DNS. Then waits the record TTL (60 s) plus 15 s. The drain time is recorded, so a resume waits only the remainder. |
| 5 | stop | 15 s | IDS expectation, then `systemctl disable --now` the app, mirror daemon, Caddy, certsync, request-rules and TURN units on the node. |
| 6 | rqlite | 30 s | Rechecks quorum, stops and disables `rqlite.service` on the node, then `DELETE /remove {"id": "nN"}` through a healthy peer, deletes its `cluster_members` row through a healthy peer (so its `/status` core row goes at once instead of paging red for a day; [../app/public-pages.md](../app/public-pages.md)), and waits until `/nodes` no longer lists it and a leader is up. Stopping first keeps a removed node from campaigning against the cluster. |
| 7 | fleet | 2 s | Removes the `[[nodes]]` block from `deploy/fleet.toml` (`remove_core_block`, the inverse of `upsert_core_block`; the rest of the file is untouched) and drops the node's `wg_pubkeys` and `rqlite_nonvoters` entries from `fleet-state.json`. |
| 8 | peers | 240 s | Every remaining core, one at a time, through `roll --steps files,start`: drain, re-render without `nN` (WireGuard peer, nftables allowances, mesh peers, certsync, rqlite node map), health gate, undrain. `wg-quick` reloads with `syncconf`, so the stale peer goes; the step then checks `wg show wg0 peers` no longer lists its key. Finished peers are recorded for a resume. |
| 9 | deregister | 10 s | Forgets `nN` in `dns-health.json`, deletes its own `nN.next.forkmesh.com` A/AAAA records, and rewrites the mail SPF record when core mail is set up. Backups, status, autodeploy and `roll` read the fleet file, so nothing else lists it. |
| 10 | commit | 2 s | Commits `deploy/fleet.toml` without the block (and with the promoted core's `rqlite_voter = false` gone) on `fleet/remove-core-nN`, based on `main`, with the same plumbing as add-core (HEAD, index and working tree untouched), and prints `git -C <repo> merge --autostash --no-ff fleet/remove-core-nN`. Nothing is pushed. |
| 11 | destroy | 30 s | Only with `--destroy-instance`. Looks the instance up by label at its provider, applies the provider's label/tag guard, requires the recorded instance id and address to match, deletes it, and forgets the label in `fleet-instances.json`. |

Without `--destroy-instance` the instance keeps running (and billing) and
stays in `fleet-instances.json`, so the desktop still lists it. Running
`remove-core nN --destroy-instance --execute` later resumes the finished run
and does only the destroy step.

### Providers and the SSH-only rule

Only the destroy step talks to a cloud provider. It picks the provider from
the instance's `provider` field in `config/fleet/fleet-instances.json`
(missing means `vultr`): through `tools/next_cloud.py` when that module is
present (the AWS provider branch), otherwise `next_fleet.Vultr`, and any
other provider is refused.

Every other step works over SSH and Cloudflare only, so a removal never
needs the Vultr or AWS key. `next_fleet.install` and `roll` normally confirm
each target against the provider API (`verify_target_instance`). For
remove-core they get an `SshIdentity` instead: it reads
`wg show wg0 public-key` on the host and requires the node's recorded
WireGuard public key. That private key exists only on the node, and SSH
already pins the host key in `config/fleet/known_hosts`, so a wrong or
reassigned address is refused just as the label check refuses it.

### Resume and recovery

- State: `config/fleet/remove-core/nN.json`, with each step's status, the
  plan (address, WireGuard key, instance id, provider, peers), the drain time,
  finished peers, branch and commit. Re-running the same command (or Retry)
  resumes from the failed step. A finished removal is refused as "already
  removed" unless `--destroy-instance` asks for the remaining destroy step.
  If the node is in the fleet file again (added back later), a new run
  starts and the old state file is renamed aside.
- A failure prints where the node is: out of DNS and held; whether it has
  left rqlite (then finish the removal rather than putting it back); peers a
  `roll` left drained. Before the rqlite step it can be put back with
  `install nN --steps start --execute` and `next_dns.py undrain nN --execute`.
- Merging: like add-core, the run edits the working tree's
  `deploy/fleet.toml` so the DNS health loop and later tool runs stop
  seeing the node at once; merge the branch with `--autostash`.
- The whole run holds `deploy.lock`.

Tests: `python3 -m pytest deploy/tests/test_next_remove_core.py` (quorum
rules, leader handover, dry-run output, the plumbing commit in a temp repo,
the identity check, and that the instance is deleted only with the flag;
fake SSH, rqlite, Cloudflare and provider; no network).

### Why a promotion step (2026-10-13)

Owner request: "bring up a new core and take one down at any time". With 3
voters, `add-core` makes `n4` a non-voter (odd voters, above). Removing any of
the original three then left 2 healthy voters and was refused, so a new core
could never replace an old one without hand work on rqlite. The promote step
turns the spare non-voter into a voter first, keeping 3 voters, so
`add-core` followed by `remove-core n1` (or any core) works with no manual
step. rqlite cannot change a member's suffrage in place: a member that
rejoins with the same id and address is ignored, so the non-voter is removed
first and rejoins with `-join` (the `--replacement` render) as a voter; its
Raft log stays on disk and catches up from the leader.

### Replace a core: add one, then remove one

```sh
cd ~/projects/forkmesh/src
python3 tools/next_fleet.py add-core                 # plan (dry run)
python3 tools/next_fleet.py add-core --execute       # nN, non-voter, in DNS (~14 min)
git merge --autostash --no-ff fleet/add-core-nN
python3 tools/next_fleet.py remove-core n1           # plan: promotes nN, then removes n1
python3 tools/next_fleet.py remove-core n1 --execute
git merge --autostash --no-ff fleet/remove-core-n1
python3 tools/next_fleet.py remove-core n1 --destroy-instance --execute   # later, when sure
```

The desktop runs the same two commands from Network → Hosts (**Add core
node**, then **Remove…** on the old core's row).

## No fixed main node

No core is special. Every role is either rendered the same for every core
from `deploy/fleet.toml` (a fleet change re-renders and rolls the cores, which
`add-core` and `remove-core` do) or chosen at run time among the live cores.
Audit of 2026-10-13, with what changed:

| Role | How it is chosen |
| --- | --- |
| rqlite bootstrap, join | Every voter renders `-bootstrap-expect <voters> -join <all voters>`; a joining or promoted core `-join`s the others. No bootstrap node. |
| rqlite leader | Raft elects it. `remove-core` hands it over first when the removed core leads. |
| Voters after a removal | `remove-core` promotes a non-voter when needed (above). |
| Migrations (`next_deploy migrate`, `next_vault`, `status_history_repair`) | The first core in fleet order whose rqlite has a leader (`find_rqlite_node`); a dead core is skipped. |
| Rollout order (`next_deploy restart`) | Changed: the first core of a rollout rotates per release (`rollout_order(fleet, rev=...)`, sha256 of `rev:name`), so no core is always the canary. Before, `n1` always went first. |
| Self-deploy (`deploy/core/self_deploy.py`) | Changed: the order is rendezvous per approval (sha256 of `id:name`), the first core to activate migrates, and a core without a fresh `cluster_members` heartbeat neither blocks the others nor holds the approval open ("not live, left for a roll"). Before, fleet order: `n1` always migrated, and a down `n1` stalled every self-deploy. |
| Backups (`fmnext-backup`) | The current Raft leader (`find_leader`). |
| Certificates (certsync) | The live core first in rendezvous order issues; the others pull. |
| Cron, durable objects, DNS reconciler, autoscaler | `rotation_owner` / rendezvous over the live `cluster_members`. |
| DNS records, health loop | Every core in `fleet.toml`; new cores are held until `undrain`. |
| WireGuard, nftables, mesh peers, certsync peers, IDS keys, git upstreams | Full mesh rendered from `fleet.toml`. |
| Runner-pool and hosted-git core lists | Changed: rendered into `app.env` as `FORKMESH_VAR_RUNNER_POOL_TARGET_NODES` and `FORKMESH_VAR_HOSTED_GIT_CORE_NODES` from the fleet's cores (`core_role_settings`), overriding the fixed `fmnext-n1,fmnext-n2,fmnext-n3` in `app/wrangler.toml` (the hosted list is empty when `[hosted] enabled = false`). The autoscaler's own default comes from the same variable, then `FORKMESH_MESH_PEERS`. |
| IDS self-stop of rqlite in a lockdown | Changed: needs the other reachable voters to be a majority of all voters (and at least 2); it was a fixed 2, which with 4 voters could stop a node whose loss broke quorum. |
| Release seeding (`forkmesh-release-host.py`, `mac_adhoc_publish.py`) | Changed: the cores come from `fleet.toml` (`fleet_core_hosts`); `config/fanout/release-hosts` keeps only its non-core lines when the fleet loads. The ad hoc publisher's tester lookup, mail and pings fall over to the next core instead of using only the first line. |
| Codemagic cores push, `next_git_host` | Pushes to every core. |
| Status page | Core rows come from `cluster_members`, runner rows from `runner_nodes`. |
| `/nodes` reads, quorum checks | The first reachable core, any will do. |

Kept as is, and why:

- The edge serves Git refs that two distinct nodes sign alike
  (`edge_routing.py`, `mirrors.PEER_QUORUM_MIN_NODES = 2`). That is a trust
  rule over signed proofs, not a core count, and it names no core.
- Durable Object rooms are still called "mainnode" in the code
  (`FORKMESH_MAINNODE_ROOM`); they are placed by rendezvous like every other
  object.
- Cores' WireGuard addresses are `10.77.0.<n>`; `certsync` and `fleet_peers`
  treat `10.77.0.21`-`29` as autoscaled replicas, so core numbers stay below 21.
- `deploy/tests/test_no_main_node.py` renders a fleet with `n4` added and one
  with `n1` removed: no rendered file of the second names `n1`, its address or
  its WireGuard IP, every voter's rqlite flags match, and the core lists
  follow the fleet.

## Node addresses

The repository is open source, so it holds no real node address: not in
`deploy/fleet.toml`, the tests, or these docs. Each node's public IPv4 and
IPv6 live only in the operator file
`~/projects/forkmesh/config/fleet/fleet-instances.json`, keyed by instance
label (`fmnext-core-1` → `{"ip": ..., "ipv6": ..., "provider": "vultr"}`),
which `create`, `create-core`, `create-runner` and `enable-ipv6` keep
current. `provider` is `vultr` or `aws`; an entry without it is a Vultr
instance, so files written before 2026-09-30 keep working.

- `render.load_fleet` fills `public_ipv4` / `public_ipv6` for every
  `[[nodes]]` and `[[runners]]` entry that has a `label` and no
  `public_ipv4`. Every tool loads the fleet through it
  (`next_common.load_fleet`, the cutover tools, `next_autodeploy.fleet_hosts`
  through `render.fill_public_addresses`), so they all see the same
  addresses. An entry may still carry `public_ipv4` inline, as
  `fleet.example.toml` and the tests do; then the operator file is not read.
- A node whose label has no `ip` in the file, or a missing file, is a
  `RenderError` naming the node and the file. The renderer never writes an
  empty address into nftables, WireGuard, DNS or the manifests.
- `FMNEXT_CONFIG_DIR` moves the whole config directory (the tests point it
  at a temp dir); `render.py --instances PATH` and `load_fleet(...,
  instances=..., instances_path=...)` name the file directly.
- `next_fleet.py fleet-toml` and the `add-core` commit write node blocks
  without addresses, and the `add-core` commit message names the label, not
  the IP.
- The release's join tree (`next_deploy.stage_join_tree`) carries
  `deploy/fleet-instances.public.json` (label → `ip`/`ipv6` only, no ids or
  plans) next to `fleet-state.public.json`, so the cores' fleet API can
  render an autoscaled node's files without the operator box.
- Tests and docs use documentation ranges: cores `203.0.113.11`..`.13`,
  runner `203.0.113.21`, retired mirrors `198.51.100.x`, and IPv6
  `2001:19f0:ffff::/48` where the renderer needs a global address
  (`2001:db8::/32` is not global, so the renderer refuses it).

## Cloud providers

Vultr was the only cloud until 2026-09-30, when Vultr had problems and AWS EC2
was added as a second provider. Both sit behind `next_cloud.Provider`. The
Vultr client is the old code with a `name` and a `guard` method; its
behaviour is unchanged. `next_fleet.create`, `verify_target_instance`,
`destroy` and `add-core` pick the client from the label's `provider` in
`fleet-instances.json`, so a mixed fleet works: an existing Vultr client
passed in is reused, and an AWS client is built from `aws.env` when a label
needs one.

### Create one instance on AWS

```sh
python3 tools/next_fleet.py create r2 --provider aws                    # dry run
python3 tools/next_fleet.py create r2 --provider aws --execute          # creates fmnext-run-2
python3 tools/next_fleet.py list --provider aws                         # read-only
python3 tools/next_fleet.py add-core --provider aws --region us-east-1  # whole core, dry run
```

`create` takes `nN`, `rN`, `aN`, `bN` or an `fmnext-<kind>-N` label, plus
`--plan` (an instance type), `--region`, `--ssh-from CIDR` (repeatable) and
`--termination-protection`. With no `aws.env` the dry run still prints the
plan offline: key pair, security group rules and instance, with the AMI shown
as resolved on `--execute`. `--execute` without credentials is an error. Like
Vultr's `create`, it adopts an instance that already carries the label.
Run `create` again after boot to record the public IP.

### What `create --provider aws` does

1. **Instance type.** The cheapest x86_64 type with at least 1 GiB of RAM
   (`MIN_RAM_MB`, the same floor as Vultr): `t3a.micro`. The fleet installs
   amd64 binaries (the rqlite tarball, the Go daemon and Debian amd64), so
   arm64 `t4g` types are refused. The 512 MB `t3a.nano`/`t3.nano` are
   accepted with `--plan`, with a warning, for a throwaway box only. Prices
   come from a built-in table of us-east-1 on-demand rates (instance + public
   IPv4 at $0.005/h + 20 GB gp3), so they are estimates: `t3a.micro` is about
   $12.11/month.
2. **Image.** The SSM public parameter
   `/aws/service/debian/release/13/latest/amd64`, then `DescribeImages` with
   owner `136693071363` (Debian). It is refused unless that is the owner, the
   architecture is x86_64 and the image is available.
3. **Key pair.** `fmnext-fleet` is imported from the fleet public key
   (`ssh/fleet/next_fleet_ed25519.pub`) and tagged `forkmesh-next=1`. An
   existing pair with that name but a different key is refused.
4. **Security group.** `fmnext-core` or `fmnext-runner` in the default VPC,
   tagged `forkmesh-next=1`. Its rules are the public part of the rendered
   nftables (`render.nftables_conf`). A core gets TCP 22, 80, 443 and the
   TURN TCP ports, UDP 443, the WireGuard port, the TURN port and the relay
   range, and ICMP types 3, 8 and 11. A runner gets TCP 22 only. SSH is open
   to `0.0.0.0/0` as on Vultr, where the node's own firewall and key-only
   sshd are the control. `--ssh-from` narrows it. An existing group without
   the tag, or with a rule nftables would not allow, is refused rather than
   changed. Missing rules are added. The ports come from
   `deploy/fleet.toml` when it loads, otherwise from the renderer's defaults.
5. **Instance.** `RunInstances` for exactly one instance with: tags
   `Name=<label>` and `forkmesh-next=1` on the instance, its volume and its
   network interface; IMDSv2 only (`HttpTokens=required`, hop limit 1, no tags
   in metadata); an encrypted 20 GB gp3 root volume deleted on termination;
   a public IPv4; `CpuCredits=standard`, so a burstable type never bills
   unlimited credits; shutdown means stop; termination protection only with
   `--termination-protection`. Its user data turns off the vendor `admin`
   user (`users: []`) and lets root log in with the fleet key
   (`disable_root: false`), as the install steps expect.

### Guards (at least as strict as Vultr's)

- Every instance must have a `Name` tag that matches `LABEL_RE`, and the tag
  `forkmesh-next` with the exact value `1`. `fleet_instances` filters on that
  tag server side and checks it again on every result. `instance`,
  `delete_instance` and `verify_target_instance` re-read live data and run
  `guard_aws_instance`: an AWS record, a well-formed `i-` id, the tag value,
  `Name` equal to the label, the Vultr label rules, and the client's region.
- `create_instance` refuses a `RunInstances` request without those tags,
  for more than one instance, without IMDSv2, or with an unencrypted root
  volume. The request is built by `run_instances_params`, which also checks
  the AMI, security group and device names.
- `destroy` still needs `--yes` and `--execute`, and the rqlite quorum guard
  still applies. An instance with termination protection on cannot be
  terminated by the tool; turn protection off in the console first.
- Snapshot builders (`fmnext-snap-N`), `enable-ipv6`, reverse DNS and the
  autoscaler stay Vultr-only.

### Credentials

`~/projects/forkmesh/config/fleet/aws.env`, mode 0600, read only through
`next_aws.load_credentials` (`common.read_env_file`, never printed;
`Credentials.__repr__` hides the secret):

```sh
AWS_ACCESS_KEY_ID=AKIA...
AWS_SECRET_ACCESS_KEY=...
AWS_REGION=us-east-1
```

`AWS_SESSION_TOKEN` is honoured, and required for an `ASIA` temporary key.
The file is plaintext on the operator disk like `vultr.env`. The fleet vault
(`DATA_KEY`) holds only secrets that the cores use, and no core calls AWS
today. The desktop can write this file from its encrypted store (Settings →
Cloud, [../desktop/networking.md](../desktop/networking.md#aws-credentials)).

### IAM policy

Create an IAM user for the fleet tools only and give it this policy, not
administrator access. Describe calls cannot be limited by resource. Every
call that creates something must carry the `forkmesh-next=1` tag. Terminate
and security group changes are allowed only on resources that already carry
it. `sts:GetCallerIdentity` (the desktop's Test button) needs no permission.

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {"Sid": "Read", "Effect": "Allow",
     "Action": ["ec2:DescribeInstances", "ec2:DescribeImages",
                "ec2:DescribeKeyPairs", "ec2:DescribeSecurityGroups"],
     "Resource": "*"},
    {"Sid": "DebianAmiParameter", "Effect": "Allow",
     "Action": "ssm:GetParameter",
     "Resource": "arn:aws:ssm:*::parameter/aws/service/debian/*"},
    {"Sid": "RunTaggedInstances", "Effect": "Allow", "Action": "ec2:RunInstances",
     "Resource": ["arn:aws:ec2:*:*:instance/*", "arn:aws:ec2:*:*:volume/*",
                  "arn:aws:ec2:*:*:network-interface/*"],
     "Condition": {"StringEquals": {"aws:RequestTag/forkmesh-next": "1"}}},
    {"Sid": "RunInstancesInputs", "Effect": "Allow", "Action": "ec2:RunInstances",
     "Resource": ["arn:aws:ec2:*::image/*", "arn:aws:ec2:*:*:subnet/*",
                  "arn:aws:ec2:*:*:security-group/*",
                  "arn:aws:ec2:*:*:key-pair/fmnext-fleet"]},
    {"Sid": "TagOnCreateOnly", "Effect": "Allow", "Action": "ec2:CreateTags",
     "Resource": "*",
     "Condition": {"StringEquals": {"ec2:CreateAction":
       ["RunInstances", "CreateSecurityGroup", "ImportKeyPair"]}}},
    {"Sid": "ImportFleetKey", "Effect": "Allow", "Action": "ec2:ImportKeyPair",
     "Resource": "arn:aws:ec2:*:*:key-pair/fmnext-fleet",
     "Condition": {"StringEquals": {"aws:RequestTag/forkmesh-next": "1"}}},
    {"Sid": "CreateTaggedGroup", "Effect": "Allow", "Action": "ec2:CreateSecurityGroup",
     "Resource": "arn:aws:ec2:*:*:security-group/*",
     "Condition": {"StringEquals": {"aws:RequestTag/forkmesh-next": "1"}}},
    {"Sid": "CreateGroupInVpc", "Effect": "Allow", "Action": "ec2:CreateSecurityGroup",
     "Resource": "arn:aws:ec2:*:*:vpc/*"},
    {"Sid": "OpenPortsOnTaggedGroups", "Effect": "Allow",
     "Action": "ec2:AuthorizeSecurityGroupIngress",
     "Resource": "arn:aws:ec2:*:*:security-group/*",
     "Condition": {"StringEquals": {"aws:ResourceTag/forkmesh-next": "1"}}},
    {"Sid": "TerminateTaggedInstances", "Effect": "Allow",
     "Action": "ec2:TerminateInstances",
     "Resource": "arn:aws:ec2:*:*:instance/*",
     "Condition": {"StringEquals": {"aws:ResourceTag/forkmesh-next": "1"}}}
  ]
}
```

To pin the user to one region, add
`"Condition": {"StringEquals": {"aws:RequestedRegion": "us-east-1"}}` to each
statement, or put it in a deny statement.

### Known gaps on AWS

- **No IPv6 by default.** The default VPC has no IPv6 range, so an AWS core
  has IPv4 only. `add-core` waits for a public IPv4 instead of both
  addresses, and DNS gets no AAAA record for that node.
- **The public IPv4 changes on stop/start** (not on reboot). Do not stop an
  AWS node. An Elastic IP would fix it, but it needs more permissions and is
  not done yet.
- **No reverse DNS.** PTR records on AWS need an Elastic IP and a request to
  AWS, so `add-core` skips the mail PTR step for AWS nodes.
- **Default VPC only.** An account without a default VPC fails at
  `CreateSecurityGroup` or `RunInstances`.
- `t3a` is missing in a few availability zones; pass `--plan t3.micro` if
  `RunInstances` says the type is unsupported.

## Target checks without the provider API (2026-10-01)

`install`, `roll` and `secrets` check an existing node by its SSH host key,
not through Vultr or AWS: `verify_target` connects with
`StrictHostKeyChecking=yes` against the pinned fleet `known_hosts` and requires
the node to answer with its own `nN.<domain>` hostname (or its own instance
label, such as `fmnext-run-1`) and the `/etc/forkmesh-next` marker. Runners keep
the instance label Vultr set as their hostname; only core nodes are renamed to
`nN.<domain>` by the mail setup. Before 2026-10-07 the check accepted only the
domain name, so `roll-runners --execute` stopped with "203.0.113.204 answers as
fmnext-run-1, not r1.next.forkmesh.com". A node whose key is not pinned yet (a brand-new
instance) still goes through `verify_target_instance` and the provider API.
The provider key is therefore needed only to create or destroy instances.
On 2026-09-30 Vultr's account API answered 502 for hours and blocked every
roll, although the nodes themselves were healthy.

## Protocols and interfaces

- Mesh between cores: WireGuard (`[wireguard]`), with addresses only from the
  static mesh map.
- rqlite Raft between the three cores; app reads go to the local node,
  writes and fresh reads through rqlite HTTP.
- Unseal: the app's unix socket, reached through `fmnext-unseal` on the node;
  the operator pipes `DATA_KEY` over SSH stdin.
- Homepage Worker to app: `x-forkmesh-edge-client`
  (`deploy/core/edge_client.md`).

## Configuration

- `deploy/fleet.toml`; operator state in `<config_dir>/fleet-state.json`
  (WireGuard public keys), secrets in `secrets.env` in the operator config dir
  (`~/projects/forkmesh/config/fleet/`, or `FMNEXT_CONFIG_DIR`), mode 0600,
  never printed.
- `FMNEXT_DIST_DIR`: where `next_deploy.py build` writes, including the
  `wheelhouse/` cache.
- `FMNEXT_SSH_MULTIPLEX=0`: one SSH connection per call instead of a shared
  master per node.
- Guard: `instance_label_prefix = "fmnext-"`; `tools/next_fleet.py` refuses
  to touch any Vultr or AWS instance without it.
- Cloud credentials: `vultr.env` (`VULTR_API_KEY`) and `aws.env`
  (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_REGION`) in the same
  folder, mode 0600 (see Cloud providers).

## Build, run, test

```sh
python3 -m pytest deploy/tests
python3 deploy/core/render.py --help
python3 tools/next_fleet.py install n1 --steps files            # dry run (default)
python3 tools/next_deploy.py all                                 # dry run
python3 tools/next_deploy.py all --execute                       # real rollout
```

Every tool is dry-run by default; `--execute` acts.

## Operations

See [OPERATIONS.md](../OPERATIONS.md): bring-up, verification, routine work
(Turnstile, native app builds, hosted repositories), recovery, destroy, cost,
known gaps, and the 2026-09-24 security hardening rollout.

## Security

- The renderer and the tools never print or render secret values.
- Runners get a per-job push capability, never core keys.
- `fmnext-ids` watches every node and can lock one down ([IDS.md](../IDS.md)).
- The production identity (`fleet.toml` `[production]`, rendered into
  `app.env` as `FORKMESH_SERVES_PRODUCTION=1` and `FORKMESH_VAR_*`) is live
  since the 2026-09-24 cutover ([CUTOVER.md](../CUTOVER.md)).

## Traps

- `fmnext-ids` takes `--config` before the subcommand
  (`deploy/core/templates/fmnext-ids.service`); `run --config` crash-loops.
- Operator units must not use `PrivateTmp` (eb14ec5).
- Nodes run Debian's Python 3.13, not 3.14; release venvs are built offline
  from a cp313 wheelhouse.
- Old releases are not pruned from `/opt/forkmesh-next/app/releases/`.
- rqlite's default `-raft-snap-wal-size` (4 MiB) made each core snapshot
  about every 3 minutes, and each snapshot rewrote the whole 70 MB database
  (`wsnapshots/*/data.db` plus the `db.sqlite` checkpoint). `rqlited` wrote
  ~60 MB/min (~86 GB/day) per core for about 5 Raft entries per second, most
  of them no-op heartbeat upserts (`cluster_members`, `dns_health_votes`,
  `host_cron_runs`, `clone_sticky`). `[rqlite] snap_wal_size` (32 MiB) and the
  optional `snap_entries` render `-raft-snap-wal-size` / `-raft-snap`, so the
  8192-entry default (~27 min at that rate) now triggers snapshots (2026-10-07).
  A roll's `files` step restarts rqlite only when `rqlite.service` or its
  drop-ins changed (the roll does one node at a time, so the others keep
  quorum). Before this, new rqlite flags never took effect, because `start`
  only runs `enable --now`. `roll_healthy` now also requires rqlite to be
  active.
- `tools/mirror_gateway.py` makes a new `forkmesh-mirror-runtime-*` temp dir
  for each repository reload (every few minutes), and it kept every retired one
  until the process exited. 3,500–5,300 had piled up in `/var/lib/forkmesh/tmp`
  on each core by 2026-10-07. A reload now cleans up retired dirs older than 15
  minutes (it keeps the newest two for in-flight responses). The
  `forkmesh-mirror-node` unit also runs `--cleanup-runtime` before it starts.
  A roll restarts the daemon (`daemon` step) before it installs new unit files
  (`files` step). So the `files` step now also restarts `forkmesh-mirror-node`
  when its unit changed. Before this fix, a unit change waited for the next
  roll.
- The `files` step never restarts `systemd-zram-setup@zram0` while
  `/dev/zram0` is swap. A restart swaps off, then zram-generator's
  `--setup-device` fails with "Can't change algorithm for initialized device"
  (EBUSY). That leaves `dev-zram0.swap` failed while the device is still in
  use, and every later transaction that pulls `swap.target` (each SSH login's
  `user@0.service`) tried again: 241 failures in 2 hours on n2 on 2026-09-24,
  plus `start-limit-hit` bursts on every roll. The step now starts the swap
  only when `/proc/swaps` lacks `/dev/zram0`. A changed
  `zram-generator.conf` takes effect at the next boot.
- `systemd-ssh-generator` (systemd 257) fails on Vultr VMs, which have no
  AF_VSOCK, and logs two err lines on every `daemon-reload`: about 260 a day
  across the fleet. The node's sshd listens on TCP only, so the `files` step
  masks the generator (`/etc/systemd/system-generators/systemd-ssh-generator`
  → `/dev/null`). That path is outside the IDS roots.
- Masking the generator left the `sshd-unix-local.socket` it had generated
  running with no unit file. After that, every `daemon-reload`, including the
  implicit one in each `systemctl enable`/`disable`, logged "Socket unit
  configuration has changed while unit has been running, no open socket file
  descriptor left" (28 times per core per 36 h, in bursts at each roll).
  Right after its `daemon-reload`, the `files` step now stops
  `sshd-unix-local.socket` and `sshd-vsock.socket` when they are still active
  with `LoadState=not-found`. sshd itself listens on TCP and is not touched.
- Caddy's global `servers` options without an address apply to every
  listener, so `protocols h1 h2 h3` also landed on the port-80 servers (the
  ACME/redirect site). On every start or reload that logged "HTTP/2 skipped
  because it requires TLS" and "HTTP/3 skipped …" warnings, and now and then
  "setting HTTP/3 Alt-Svc header … no port can be announced" at error level.
  The Caddyfile now scopes `servers :443` (h1 h2 h3) and `servers :80` (h1),
  with the same timeouts and header limit on both.

## Recent changes

- 2026-10-04: cores merge each other's `main`. Two gaps kept a core's own
  write to `main` (a runner merge on n1, `a65a60890`) away from the other
  cores for 11 hours. First, peers were read only when the edge failed, and
  the edge served the state two cores agreed on, which was not n1's. Second,
  when two `main`s diverged with identical issue trees, `reconcileUpstreamRefs`
  kept the newer commit and dropped the other side's commits from `main`.
  Changes in `server/gateway.go`:
  - After a successful sync, `convergePeerMains` fetches the HEAD branch
    (`main`) from every upstream after the one used, which for a core means
    every other core's own host, at most every 30 s per repository
    (`peerMainInterval`). It stages the result under `refs/forkmesh/peer/` and
    then clears it. A peer that is behind changes nothing, and a peer that is
    ahead fast-forwards the core.
  - `chooseRefValue` is the one rule for both the edge fetch and the peer
    fetch. For the HEAD branch, a code-only divergence that `git merge-tree`
    merges cleanly, with at most 400 commits on each side of the merge base
    (`maxMainMergeSideCommits`), becomes a merge commit, "Merge divergent core
    main". It uses the issue reconciler's deterministic form
    (`writeReconcilerMerge`: sorted parents, author ForkMesh Mirror
    Reconciler, date one second after the newer parent), so every core writes
    the same commit id and two cores agree on it again, which lets the edge
    pin it.
  - A conflicting divergence, a divergence deeper than 400 commits (for
    example a deliberate history rewrite such as the banned-identifier
    purge), and every branch other than HEAD keep the old newer-commit rule.
    Issue-tree divergence is unchanged.
  - Trust: peers were already configured upstreams (2026-09-28), so they could
    already replace `main` with a newer commit. Merging asks no more trust of
    them than that.
  - Tests: `server/core_peer_converge_test.go`.
  - Takes effect on the next `daemon` deploy step.
- 2026-10-01: `next_fleet.py remove-core` and the desktop's **Remove…**
  button (see Removing a core node). Supporting changes:
  `next_fleet.remove_core_block`; `verify_target_instance` accepts a verifier
  with `verify_target`, which remove-core uses to check nodes over SSH;
  `next_add_core.Git.commit_file` also accepts `fleet/remove-core-nN`.
- 2026-09-30: AWS EC2 is a second cloud provider (see Cloud providers):
  `next_cloud.py` (interface), `next_aws.py` (SigV4 and EC2 with the
  standard library only; there was no SigV4 helper in the repository and no
  vendored boto3), `next_fleet.py create <target> --provider aws`, a
  `provider` field in `fleet-instances.json`, and `--provider` on
  `add-core`. Tests: `deploy/tests/test_next_aws.py` (AWS test-suite
  signature vectors, request building over a fake transport, guards, the
  provider field, add-core options).
- 2026-09-30: `next_fleet.py add-core` and the desktop's **Add core node**
  button (see Adding a core node). Supporting changes: `render_core` honours
  `rqlite_voter = false` and counts only voters in `-bootstrap-expect` and
  `-join`; `fleet_toml_text` keeps `fleet-state.json` `rqlite_nonvoters`;
  `create` can turn IPv6 on at creation; `next_dns` holds a newly seen core
  out of DNS, and `ensure` skips held cores. `fleet-toml --execute`
  regenerates the whole file and drops keys it does not know
  (`flagship_receive_pack`, `[autoscale]` today), which is why add-core
  inserts one block instead.
- 2026-09-28: core peer upstreams. Each core's mirror-node used to have one
  upstream for `forkmesh/forkmesh`, the public edge URL that `--init` wrote.
  At 17:45Z the three cores each wrote their own issue #729 intake to main
  and diverged. The edge then answered every git read with
  `503 mirror_unavailable`, and because the edge was the cores' only
  upstream, they could not fetch from each other to heal. `daemon_overrides`
  (`deploy/core/render.py`) now renders `upstreams` for a core: the public
  URL first (still the catalog clone URL and the only `eventDrivenUpstreams`
  entry), then every other core's own gateway,
  `https://nN.next.forkmesh.com/forkmesh/forkmesh.git`. `syncRepository`
  (`server/gateway.go`) tries them in order and stops at the first fetch that
  succeeds, so a peer is used only when the edge fails. The per-node Caddy
  site proxies anonymous reads straight to that core's gateway, which serves
  its current refs. Takes effect on the next `daemon` step, which merges the
  overrides over `/etc/forkmesh/mirror-node.json`.
- 2026-09-27: runner OOM fix: the runner daemon's `/tmp` is on disk
  (`BindPaths=`, not a tmpfs `PrivateTmp`), jobs may swap
  (`jobMemorySwapMax=1G`), and the `files` step stops the orphaned
  `sshd-unix-local.socket` ([../RUNNERS.md](../RUNNERS.md) "Memory on the 1 GB
  runner"). Roll the runner with `next_fleet.py install r1 --steps
  files,daemon,start --execute`, so the daemon restarts with the new unit.
- 2026-09-27: `fleet.toml` `[app] d1_log` (default true) renders
  `FORKMESH_HOST_D1_LOG` into every core's `app.env`, so the app journals each
  D1 statement and the desktop shows it under DB
  ([../desktop/networking.md](../desktop/networking.md)). Takes effect on the
  next deploy.
- 2026-09-26: faster rollouts: shared SSH connections, parallel uploads with
  the migration alongside, venv reuse, wheelhouse cache, batched schema replay
  (see Deploy speed).
- 2026-09-24: log-error pass: the `files` step no longer restarts live zram
  swap and masks `systemd-ssh-generator`; Caddy protocols are scoped per
  port (see Traps).
- 2026-09-24: IDS rolled out and enabled (6df712f, 863ed6a); hosted
  repositories on the core (32b946a, 7fe3a76); private Git for Codemagic
  (068e876); security hardening of Caddy, certsync, nftables and units
  (3cf01fa); leaderless cron/DNS/certificates (ac7c6d3); age-encrypted
  backups (0ba6371); production cutover tooling (3e432a2).
- 2026-09-23: phases 4, 6, 7: core packaging and fleet/deploy/DNS/backup
  tooling (0b0ec8b); runner tier (e92f6b3); sealed vault (b8b95bf).

## Related docs

[PLAN.md](../PLAN.md) · [OPERATIONS.md](../OPERATIONS.md) ·
[VAULT.md](../VAULT.md) · [IDS.md](../IDS.md) · [RUNNERS.md](../RUNNERS.md) ·
[REKEY.md](../REKEY.md) · [HOSTED_REPOS.md](../HOSTED_REPOS.md) ·
[CUTOVER.md](../CUTOVER.md) · [docs/host/README.md](../host/README.md)
