# Deploy: the core fleet

`deploy/` turns one fleet description, `deploy/fleet.toml`, into every file a
fleet node needs. The operator tools in `tools/next_*.py` create instances,
install nodes, push secrets and roll out releases. Node kinds: **core** nodes
(trusted: Caddy, the app on CPython, rqlite, the Go daemon and Git gateway)
and **runners** (untrusted, outbound-only agent compute). The runbook is
[OPERATIONS.md](../OPERATIONS.md); this page is the map.

## Code map

| Path | Responsibility |
| --- | --- |
| `deploy/fleet.toml` | the live fleet: `[[nodes]]`, `[[runners]]`, `[paths]`, `[wireguard]`, `[rqlite]`, `[caddy]`, `[app]`, `[production]`, `[autoscale]`, `[turn]`, `[calls]`, `[dns]`, `[ids]`, `[self_deploy]`, `[logs]`, `[hosted]`. No secrets, no addresses. |
| `deploy/fleet.example.toml` | annotated template |
| `deploy/core/render.py` | renders a node's files into a tree plus `MANIFEST.json`; never reads secrets |
| `deploy/core/caddy_routes.py` | Caddy route snippets shipped with each release |
| `deploy/core/request_rules.py` | renders request block rules into a Caddy include |
| `deploy/core/certsync.py` | leaderless certificate sharing |
| `deploy/core/ids.py` | host intrusion detection ([IDS.md](../IDS.md)) |
| `deploy/core/self_deploy.py` | phone-approved merges deployed by the cores ([self-deploy.md](self-deploy.md)) |
| `deploy/core/edge_client.md` | wire contract for `x-forkmesh-edge-client` |
| `deploy/core/templates/`, `deploy/runner/templates/` | unit files and configs |
| `deploy/operator/` | operator-box user units: backup, unseal, DNS health |
| `deploy/tests/` | renderer, Caddy, certsync, IDS and tool tests |
| `tools/next_common.py` | SSH, fleet loading, dry-run plumbing, ops log tee |
| `tools/next_fleet.py` | create, install, secrets, roll, add/remove/replace core, destroy |
| `tools/next_cloud.py`, `tools/next_aws.py` | provider interface; AWS EC2 provider (standard-library SigV4) |
| `tools/next_add_core.py`, `tools/next_remove_core.py` | resumable add/remove runs |
| `tools/next_deploy.py` | `build`, `upload`, `migrate`, `restart`, `all`, `rollback` |
| `tools/next_autodeploy.py` | auto-deploy of `main` ([auto-deploy.md](auto-deploy.md)) |
| `tools/next_vault.py`, `next_ids.py`, `next_dns.py`, `next_backup.py`, `next_rekey.py`, `next_git_host.py`, `next_codemagic.py` | vault, IDS, DNS, backup, re-key, private Git and native builds |

## Architecture

- **Render, then push.** `render.py` produces files only;
  `next_fleet.py install <node> --steps …` copies them and runs each step over
  SSH. Core steps: `base`, `wireguard`, `rqlite`, `turn`, `calls`, `files`,
  `secrets`, `start` (plus `daemon`, `caddy`, `ids`, `logread`). Runner steps: `base`, `files`,
  `start`, `daemon`.
- **Secrets never rendered.** Units load encrypted credentials by name
  (`LoadCredentialEncrypted=`). With `[app] vault = true` the app gets only
  bootstrap credentials and starts sealed ([VAULT.md](../VAULT.md)).
- **Releases.** `next_deploy.py build` makes a tarball (app host and source,
  built assets, World, an offline venv, Caddy snippets, `BUILD_REV`) that lands
  in `/opt/forkmesh-next/app/releases/<rev>` with `current` and `previous`
  symlinks. The operator box keeps the `KEEP_RELEASES` newest tarballs.
- **Rolling restart gate.** Each node must report `/readyz` and the new
  `BUILD_REV`; a failing node goes back to its previous release and the rollout
  stops. Migrations run once, through any healthy rqlite node.
- **A core down does not block a release.** `upload`, `restart` and `all`
  leave out a core that is held or out of DNS (`dns-health.json`, the
  `next_dns.py` health loop) or that SSH cannot reach, as long as a majority
  of the cores and at least two still get it (`deploy_quorum`); otherwise
  they refuse. `--skip nN` names the cores to leave out instead. The deploy
  manifest records `skipped`, and `restart nN` of that release later clears
  it (`caughtUp`). See [auto-deploy.md](auto-deploy.md), "Skipped cores".
- **Speed.** One multiplexed SSH connection per node
  (`FMNEXT_SSH_MULTIPLEX=0` disables it), parallel uploads with the migration
  alongside, venv reuse when requirements are unchanged, a wheelhouse cache,
  and batched schema replay.

## No fixed main node

No core is special. Every role is rendered identically from `fleet.toml` or
chosen at run time among live cores:

| Role | How it is chosen |
| --- | --- |
| rqlite bootstrap and join | every voter renders the same `-bootstrap-expect` and `-join` |
| rqlite leader | Raft; `remove-core` hands it over first |
| migrations | first core with a healthy rqlite |
| rollout order | rotates per release (`rollout_order`, sha256 of `rev:name`) |
| self-deploy order | rendezvous per approval |
| backups | the current Raft leader |
| certificates, cron, Durable Objects, autoscaler | rendezvous or rotation over live `cluster_members` |
| WireGuard, firewall, mesh, certsync, IDS keys, Git upstreams | full mesh from `fleet.toml` |
| runner-pool and hosted-git core lists | rendered into `app.env` from the fleet's cores |
| group calls | every core runs `fmnext-calls`; the app places each room by rendezvous over the healthy cores in `FORKMESH_CALLS_NODES` ([calls.md](calls.md)) |

`deploy/tests/test_no_main_node.py` checks this.

## Adding a core node

`python3 tools/next_fleet.py add-core [--region R] [--plan P] [--provider P] --execute`
(the desktop's **Add core node** runs the same). Without `--execute` it prints
the plan. Steps:

| # | Step | What it does |
|---|---|---|
| 1 | plan | next number, region and plan, rqlite role |
| 2 | hold | mark the node `held` in `dns-health.json` before anything names it |
| 3 | vm | create or adopt the labelled instance (IPv6 at creation) |
| 4 | fleet | upsert the `[[nodes]]` block |
| 5 | keys | WireGuard and IDS keys generated on the node |
| 6 | mesh | re-render peers one at a time while the new core installs |
| 7 | join | start; rqlite joins over the mesh; the IDS starts last |
| 8 | services | daemon, calls, files, start; sync every repository from peers |
| 9 | activate | deploy the served release, unseal |
| 10 | repos | provision the core's node account, then wait until every repository is present and synced |
| 11 | ids | install the log reader, sign the baseline |
| 12 | health | node and external health checks |
| 13 | dns | undrain after probes |
| 14 | register | confirm DNS release and the node account |
| 15 | commit | commit `deploy/fleet.toml` on `fleet/add-core-nN`, then merge it into `main` |

- **rqlite role:** keep an odd number of voters; with three voters a fourth
  core joins as a non-voter (`rqlite_voter = false`).
- **DNS:** a newly seen core is held out of DNS until an explicit undrain.
- **Resume:** state in `config/fleet/add-core/nN.json`; every step is
  idempotent; a failure drains the node and keeps the VM for a retry.
- **Node account before repos:** without its `fmnext-nN` node account the relay
  answers the new daemon with HTTP 401, so repositories it learns from the
  catalog never sync. On 2026-10-23 n5 waited 20 minutes for `kaifdemo/demo`.
  `repos` registers the account first (idempotent) and nudges a sync.
- **Snapshots:** only keyless builder images are accepted; cloning a live
  core would copy its keys and Raft state.
- **Merge:** the commit step merges the branch into `main` itself (see
  [Landing the fleet change on main](#landing-the-fleet-change-on-main)).

## Removing a core node

`python3 tools/next_fleet.py remove-core nN [--destroy-instance] --execute`
(the desktop's **Remove…**). Steps: preflight (refused below three cores or
three healthy voters), promote a non-voter if voters would drop below three
or become even, hand over leadership, drain from DNS, stop services, leave
rqlite and delete its membership row, remove the fleet block, re-render
peers, deregister DNS names, commit on `fleet/remove-core-nN` and merge it into
`main`, and optionally destroy the instance.

Only the destroy step talks to a cloud provider. Other steps identify the
node over SSH by its pinned host key and recorded WireGuard public key. State
is in `config/fleet/remove-core/nN.json`; runs resume.

### Landing the fleet change on main

Autodeploy, roll and backups read `deploy/fleet.toml` on `main`, and every
deploy first asks each core listed there for `/api/version`. A fleet change
left on its branch breaks every deploy: on 2026-10-23 n2 was removed and n5
added, neither branch was merged, and each deploy from 03:21 on (run 7110 and
the runs before it) failed with "n2 did not answer /api/version (HTTP 0)".

So the commit step of `add-core` and `remove-core` lands the change itself
(`land_on_main` in `tools/next_add_core.py`):

1. `git merge-tree --write-tree main <commit>` first; a conflict is refused
   and nothing is touched.
2. Where `main` is checked out (normally `~/projects/forkmesh/src`), it runs
   `git merge --no-ff` there. A changed file holding exactly the merge result
   (the tool's own edit of `deploy/fleet.toml`) is set back to `main`'s copy
   first and rewritten if the merge fails. Any other unsaved or staged change
   to that file refuses the merge. Unrelated unsaved work stays as it is, and
   no stash is used.
3. When no checkout holds `main`, it writes the merge commit with plumbing and
   moves `refs/heads/main` by compare-and-swap.

The merge in the source checkout is published like any other commit
([source-publisher.md](../tools/source-publisher.md)), and that push starts
the next deploy. A refused merge does not fail the run (the core is already
added or removed): the run records `merge_error` and prints the
`git -C <main checkout> merge --no-ff <branch>` to run. Until then, gate 1
names the pending branch: autodeploy lists `fleet/add-core-nN` branches whose
core is not in `main`'s file and `fleet/remove-core-nN` branches whose core
still is (`unmerged_fleet_branches` in `tools/next_autodeploy.py`).

## Replacing a core node

`python3 tools/next_fleet.py add-core --replace nOLD [--region R] [--plan P] --execute`
(the desktop's **Replace…** on a core row in Network, Hosts) adds a new core
and then takes `nOLD` out in the same run, so the fleet never drops below
three cores and there is one fleet commit, which lands on `main` like any
add or remove.

- The add steps run as usual. A `remove` step before `commit` then runs
  `remove-core nOLD`: it promotes the new core to a voter if needed, drains,
  stops and removes `nOLD`, and re-renders the peers.
- If the `remove` step fails, the new core stays serving and in DNS. A re-run
  (or **Retry**) resumes the removal from its own state file. Failures before
  `remove` drain the new core, as in a plain add.
- `commit` writes one commit on `fleet/add-core-nNEW` with the new block (made a
  voter if it was promoted), without the `nOLD` block, and with `nOLD` in
  `[hosted] retired_cores`. The removal's own `fleet/remove-core-nOLD` commit is
  skipped.
- The old instance is kept. Delete it later with
  `remove-core nOLD --destroy-instance --execute` (**Remove…** with the instance
  box ticked), which resumes the finished removal and runs only `destroy`.
- `add-core-options --replace nOLD --json` gives the step list and estimate;
  `add-core-status --json` includes `replaces` and `step_list`.

The manual route still works: `add-core`, then `remove-core <old>`; each
lands its own commit on `main`.
The legacy `replace-core nOLD --new nNEW` is the older, partly manual path.

## Node addresses

The repository holds no real node address. Addresses live only in
`~/projects/forkmesh/config/fleet/fleet-instances.json`, keyed by label
(`{"ip", "ipv6", "provider"}`). `render.load_fleet` fills them in; a missing
address is a `RenderError` naming the node and that operator file, and nothing
renders an empty address. Tests and docs use documentation ranges
(`203.0.113.0/24`, `198.51.100.0/24`, `2001:19f0:ffff::/48`).

### Reaching nodes over IPv6

On 2026-10-07 the operator box lost IPv4 while IPv6 kept working, and a deploy
stopped mid-rollout. Node configs, firewall rules and DNS A records keep using
`ip`; only the operator's own connections can switch address family. Those are
`next_common.Ssh` (ssh, scp, tunnels) and the autodeploy `/api/version` probe,
and both go through `next_common.route_address`. `FMNEXT_ROUTE_FAMILY` picks
the family:

- `auto` (the default): use `ip` while it answers on port 22. If it doesn't
  and the node's `ipv6` does, use `ipv6`. The choice is made once per process.
- `4`: always use `ip`.
- `6`: use `ipv6` for every node that has one.

Over IPv6, ssh passes `-o HostKeyAlias=<ip>`, so the host key pinned under the
IPv4 address is still checked; no new key is accepted. A node without `ipv6`
(the runner today) always uses `ip`. Tests that inject a fake runner never
route.

## Cloud providers

Vultr and AWS EC2 sit behind `next_cloud.Provider`; the label's `provider`
field selects the client, so a mixed fleet works.

`python3 tools/next_fleet.py create r2 --provider aws [--execute]` picks the
cheapest x86_64 type with at least 1 GiB, the official Debian 13 image
(owner-checked), imports the fleet key pair, creates a tagged security group
mirroring the public nftables rules, and launches exactly one instance with
IMDSv2 only, an encrypted root volume, standard CPU credits, and tags
`Name=<label>` and `forkmesh-next=1`.

Guards: every instance must carry a `Name` matching the label rules and the
tag `forkmesh-next=1`, re-checked on every read and before every delete;
`destroy` needs `--yes` and `--execute` and respects the quorum guard.

Credentials: `config/fleet/aws.env` (0600) with `AWS_ACCESS_KEY_ID`,
`AWS_SECRET_ACCESS_KEY`, `AWS_REGION` (and `AWS_SESSION_TOKEN` for temporary
keys). The desktop can write it from Settings, Cloud.

### IAM policy

Give the fleet tools a dedicated IAM user with this policy, not administrator
access:

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

Known AWS gaps: no IPv6 in the default VPC, the public IPv4 changes on
stop/start (do not stop AWS nodes), no reverse DNS, default VPC required.
Snapshot builders, `enable-ipv6`, reverse DNS and the autoscaler are
Vultr-only.

## Target checks

`install`, `roll` and `secrets` verify an existing node by its pinned SSH host
key, its own hostname or instance label, and the `/etc/forkmesh-next` marker,
so provider API outages do not block rolls. The provider key is needed only to
create or destroy instances.

## Protocols and configuration

- Cores mesh over WireGuard; rqlite Raft between voters; the app reads
  locally and writes through rqlite HTTP.
- Unseal: `DATA_KEY` over SSH stdin to the app's unix socket.
- The homepage Worker signs `x-forkmesh-edge-client`.
- Operator config in `~/projects/forkmesh/config/fleet/` (`FMNEXT_CONFIG_DIR`);
  `FMNEXT_DIST_DIR` for builds; the label prefix guard
  `instance_label_prefix = "fmnext-"` cannot be bypassed.

## Build, run, test

```sh
python3 -m pytest deploy/tests
python3 deploy/core/render.py --help
python3 tools/next_fleet.py install n1 --steps files      # dry run
python3 tools/next_deploy.py all --execute
```

## Services on every core

- Office TURN relay: `fmnext-turn.service`, `[turn]`
  ([office/README.md](../office/README.md)).
- Group-call server (SFU): `fmnext-calls.service`, `[calls]`, UDP 3479 and
  `/calls/*` on each per-node name ([calls.md](calls.md)).

## Traps

- `fmnext-ids` takes `--config` before the subcommand.
- A fleet branch that is not merged into `main` makes every deploy fail on
  gate 1 ([Landing the fleet change on main](#landing-the-fleet-change-on-main)).
- Operator units must not use `PrivateTmp`.
- rqlite's default snapshot WAL size makes small databases snapshot every few
  minutes and rewrite the whole file; `[rqlite] snap_wal_size` (and optional
  `snap_entries`) raise it. The `files` step restarts rqlite only when its unit
  changed, one node at a time.
- The `files` step restarts `forkmesh-mirror-node` when its unit changed, never
  restarts zram swap while it is active, masks the systemd SSH generator on
  VMs without vsock, and stops orphaned generated sockets.
- Caddy's global server options are scoped per listener (`:443` with h1/h2/h3,
  `:80` with h1).
- Cores fetch `main` from each other after each sync; a clean code-only
  divergence becomes a deterministic merge commit so cores converge
  (`server/gateway.go`, `convergePeerMains`).
- Memory on a 1 GB core (2026-10-16 survey: 650-740 MiB used, 220-320 MiB
  available). rqlite runs with `MemoryHigh=260M`, `MemoryMax=320M`,
  `GOMEMLIMIT=150MiB` and no swap. At the old `MemoryMax=220M` every core's
  rqlite sat at its limit (thousands of `memory.events` max hits; the page cache
  of the 99 MB database counts against the unit), and on 2026-10-16 the leader
  on n3 was OOM-killed under a burst of analytics reads. `MemoryHigh` makes the
  kernel reclaim first instead of killing.
- `forkmesh-mirror-node` (`MemoryHigh=96M`, `MemoryMax=128M`) also holds its
  `mirror_gateway.py` child. The gateway's in-memory caches were allowed 122 MiB
  (responses 64, dashboard assets 48, catalog 8, sessions 2), more than the
  whole unit, so after two days each core carried about 185 MiB of the gateway
  in swap. The caps are now responses 24 MiB, assets 12 MiB; entries in those
  two caches are counted with 1 KiB of overhead (`entry_overhead`) because
  the wrapper and headers are not in the body length; finished merge-job
  records are capped at 1,000.
- The gateway hashes a release blob before serving it. A verified
  (path, size, mtime, inode) is remembered for an hour (512 entries), so the
  `/status` download checks and repeat downloads do not re-read 60 MB files.
  The checks themselves send `HEAD` (`status_monitoring.py`): they need only
  the status, and a `GET` whose body was cancelled made the serving core
  stream part of each blob every minute (Caddy's "aborting with incomplete
  response").
