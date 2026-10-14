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
| `deploy/fleet.toml` | the live fleet: `[[nodes]]`, `[[runners]]`, `[paths]`, `[wireguard]`, `[rqlite]`, `[caddy]`, `[app]`, `[production]`, `[dns]`, `[ids]`, `[logs]`, `[hosted]`. No secrets, no addresses. |
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
  SSH. Core steps: `base`, `wireguard`, `rqlite`, `files`, `secrets`, `start`
  (plus `daemon`, `caddy`, `ids`, `logread`). Runner steps: `base`, `files`,
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
| 8 | services | daemon, files, start; sync every repository from peers |
| 9 | activate | deploy the served release, unseal |
| 10 | repos | wait until every repository is present and synced |
| 11 | ids | install the log reader, sign the baseline |
| 12 | health | node and external health checks |
| 13 | dns | undrain after probes |
| 14 | register | confirm DNS release; provision the core's node account |
| 15 | commit | commit `deploy/fleet.toml` on `fleet/add-core-nN` without touching HEAD |

- **rqlite role:** keep an odd number of voters; with three voters a fourth
  core joins as a non-voter (`rqlite_voter = false`).
- **DNS:** a newly seen core is held out of DNS until an explicit undrain.
- **Resume:** state in `config/fleet/add-core/nN.json`; every step is
  idempotent; a failure drains the node and keeps the VM for a retry.
- **Snapshots:** only keyless builder images are accepted; cloning a live
  core would copy its keys and Raft state.
- **Merge** with `git merge --autostash --no-ff fleet/add-core-nN` (the working
  tree already holds the same file).

## Removing a core node

`python3 tools/next_fleet.py remove-core nN [--destroy-instance] --execute`
(the desktop's **Remove…**). Steps: preflight (refused below three cores or
three healthy voters), promote a non-voter if voters would drop below three
or become even, hand over leadership, drain from DNS, stop services, leave
rqlite and delete its membership row, remove the fleet block, re-render
peers, deregister DNS names, commit on `fleet/remove-core-nN`, and optionally
destroy the instance.

Only the destroy step talks to a cloud provider. Other steps identify the
node over SSH by its pinned host key and recorded WireGuard public key. State
is in `config/fleet/remove-core/nN.json`; runs resume.

**Replace a core:** `add-core`, merge, `remove-core <old>`, merge, then
`--destroy-instance` when sure.

## Node addresses

The repository holds no real node address. Addresses live only in
`~/projects/forkmesh/config/fleet/fleet-instances.json`, keyed by label
(`{"ip", "ipv6", "provider"}`). `render.load_fleet` fills them in; a missing
address is a `RenderError` naming the node and that operator file, and nothing
renders an empty address. Tests and docs use documentation ranges
(`203.0.113.0/24`, `198.51.100.0/24`, `2001:19f0:ffff::/48`).

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

## Traps

- `fmnext-ids` takes `--config` before the subcommand.
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
