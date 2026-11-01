# Operations

app.forkmesh.com and world.forkmesh.com are served by three equal,
self-hosted core nodes (Caddy, the Python app on CPython, rqlite and the Go
Git gateway), reached directly through DNS round robin, plus an untrusted
runner tier for agent work. There is no proxy in front: a failed core leaves
DNS, and with two cores down the survivor serves read-only. The public picture
is [architecture](../architecture.md); the design in detail is the
[served stack](served-stack.md), and the commands are in the
[runbook](runbook.md).

## Run the fleet

| Page | Covers |
| --- | --- |
| [Served stack](served-stack.md) | Who serves what, the core and runner design, read-only mode, performance rules |
| [Runbook](runbook.md) | Bring up, verify, routine operations, DNS failover, recovery, destroy, hardening |
| [Fleet tooling](fleet-tooling.md) | The `deploy/` and `tools/next_*.py` code map; adding, removing and replacing cores; cloud providers |
| [Operator files](operator-files.md) | Where the operator box keeps fleet secrets, keys, state and logs |
| [Autoscaling](autoscaling.md) | App replicas and burst runners scaled by the cores under signed settings |
| [Runners](runners.md) | The untrusted runner tier: job flow, security model, personal and macOS runners |
| [Fleet SSH port](ssh-port.md) | sshd on port 822 on every fleet node, and moving a node |
| [Node logs](node-logs.md) | Live node journals and Caddy logs on the operator desktop |
| [Node ownership](node-ownership.md) | Node accounts, their operator, and version fields |
| [Call servers](call-servers.md) | The group-call server (SFU) on every core |

## Deploy and release

| Page | Covers |
| --- | --- |
| [Auto-deploy](auto-deploy.md) | A merge to `main` deploys the cores from the operator box, with gates and skipped cores |
| [Self-deploy](self-deploy.md) | The cores deploy phone-approved merges while the operator box is offline |
| [Self-hosted instances](instances.md) | Start, install, update, theme and run a white-label instance |
| [Deployment and scheduled automation](automation-and-cloudflare-bootstrap.md) | Workers self-host bootstrap, issue sync, daily security scan, reward scheduling |
| [Releases](../release/README.md) | Versions, builds, signing, promotion, apt and installers |

## Mirrors and gateways

| Page | Covers |
| --- | --- |
| [Mirror node](mirror-node.md) | The `forkmesh-mirror-node` daemon: sync, catalog, web merges, agent jobs |
| [Direct-HTTPS mirror gateway](direct-https-mirror-gateway.md) | `mirror_gateway.py`: security boundary, independent operation, routing contract, operations |
| [Headless mirror identity and refresh](headless-mirror.md) | The identity helper and the refresh tool for a non-GUI mirror host |
| [Git SSH gateway](git-ssh-gateway.md) | SSH keys, the OpenSSH origin, relay mode, controller-free operation |
| [Mirror Actions control](mirror-actions-control.md) | The wire contract for the Actions tab on a mirror |
| [Node operator policy](node-operator-policy.md) | Per-node storage limits, takedowns, protected branches and legal response |
| [Node backup and recovery](node-backup-and-recovery.md) | Node backups, restore drills and identity key moves |
| [Breakout authorization replica](breakout-authorization-replica.md) | Signed authorization data that lets a mirror work while the cores are unreachable |
| [Service roles](service-roles.md) | The eight self-hostable roles, their substitutes and checks |

## Repositories and pull requests

| Page | Covers |
| --- | --- |
| [Core push](core-push.md) | Owner pushes to the cores: who can push, ref rules, force pushes, consistency |
| [Hosted repositories](core-hosted-repositories.md) | Repositories hosted on the cores: design, forks, push mirrors, maintenance |
| [Pull request rules](pull-rules.md) | Blocking and warning rules for pull requests, and the open pull audit method |
| [Signed required checks](required-checks.md) | Signed status checks that gate web merges |
| [Git conformance](git-conformance.md) | Evidence and release gate behind the Git compatibility matrix |

## Security and secrets

| Page | Covers |
| --- | --- |
| [Vault](vault.md) | `DATA_KEY`, sealed nodes, peer unseal and secret rotation |
| [Re-keying](rekey.md) | Offline `DATA_KEY` rotation |
| [Intrusion detection](intrusion-detection.md) | `fmnext-ids`: detectors, lockdown, alerts, quarantine |
| [Disaster recovery](../disaster-recovery/README.md) | Community recovery: roster, bootstrap kit, printable runbook, drills |

## Accounts and services

| Page | Covers |
| --- | --- |
| [Mail log](mail-log.md) | The #email-log channel, tracking, daily budget and sending from the cores |
| [Mobile push notifications](mobile-push-notifications.md) | APNs alerts that wake the iOS app |
| [Polling elimination](polling-elimination.md) | Push over WebSockets instead of client polling |
| [Desktop control node](desktop-control-node.md) | The desktop's Control page: export, import, keys, API token, hosts |
| [Organization succession](organization-succession.md) | Non-custodial owner succession for one organization |
| [Organization agent offline queue](organization-agent-offline-queue.md) | Agent work queued for an offline organization desktop |
| [ForkMesh Ads](ad-network.md) | The first-party, reviewed, text-only ad network |

## Payments and rewards

| Page | Covers |
| --- | --- |
| [Community reward pool](community-reward-pool.md) | The non-custodial pool, randomized mirror rewards, and the desktop's local signer |
| [Paid microjobs](microjobs.md) | Discretionary SOL rewards for issues and pull requests |
| [Custodial element deposits](custodial-element-deposits.md) | Optional deposit addresses for World element purchases |
| [Legacy Solana custody migration](legacy-solana-custody-migration.md) | Offline migration and scrub of old custodial wallet seeds |
| [Patreon](patreon.md) | Membership payments through Patreon |
| [Plan trials](plan-trials.md) | Trials and self-serve usage meters |
| [Business-model rollout](business-model-rollout.md) | Release gate, failure paths and rollback for paid services |

Core pages: [architecture](../architecture.md),
[encryption](../encryption.md), [mirroring](../mirroring.md),
[self-hosting](../self-hosting.md),
[censorship resilience](../censorship-resilience.md) and the
[threat model](../protocol/threat-model.md). Back to the
[documentation home](../README.md).
