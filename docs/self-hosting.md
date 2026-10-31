# Self-hosting

Everything ForkMesh runs is open source and can run on your own machines. Pick
the level you need: a node on your computer, a mirror on a server, a whole
instance under your own name, or a multi-node fleet like forkmesh.com.

Part of the core documentation: [architecture](architecture.md) ·
[encryption](encryption.md) · [mirroring and distribution](mirroring.md) ·
[self-hosting](self-hosting.md) · [censorship resilience](censorship-resilience.md)
· [threat model](protocol/threat-model.md).

## Choose a level

| Level | You get | Needs | Time | Guide |
| --- | --- | --- | --- | --- |
| Desktop node | Your repositories hosted and served from your computer, coding agents, chat | Linux, macOS or Windows | not measured | [getting started](getting-started.md) |
| Mirror | A server that carries copies of chosen repositories and serves clones | A Linux server with a public HTTPS name | not measured | [mirror node](operations/mirror-node.md) |
| Instance | Your own ForkMesh: accounts, repositories, pull requests, chat, Actions, your domain and branding | Debian 13 or Ubuntu 24.04, 1 GB RAM, 6 GB disk, ports 80 and 443 | about 15 minutes | [start your own instance](start-your-own-instance.md) |
| Fleet | Several equal core nodes behind DNS round robin, a runner tier, sealed secrets | Three servers plus an operator machine | not measured | `docs/operations/README.md` |

## Desktop node

```sh
curl -fsSL https://forkmesh.com/install.sh | bash
```

The installer verifies the signed release manifest before it installs
anything ([encryption](encryption.md#software-distribution)). Windows and macOS
packages are on [forkmesh.com/desktop](https://forkmesh.com/desktop); supported
platforms are in the [installation support matrix](install-support-matrix.md).
The desktop is a full node: it holds bare repositories, serves clones and signs
records with its own key ([desktop app guide](qt-client.md)).

## Mirror

Two ways in:

- **From the desktop.** Add a server in the desktop's host list; it installs and
  updates `forkmesh-mirror-node` over SSH and pushes your repositories to it.
- **Without the desktop.** One command on the server installs the service,
  links it to your account, waits for your DNS record and serves it over HTTPS
  with a Let's Encrypt certificate:

  ```sh
  curl -fsSL https://forkmesh.com/install.sh | \
    FORKMESH_PUBLIC_HOSTNAME=<host> FORKMESH_LINK_CODE=<code> FORKMESH_OWNER=<user> bash
  ```

  The six-digit link code comes from your account and expires after 30 minutes.
  The same script works as cloud-init user data. Detail:
  `docs/release/self-setup-node.md`.

A mirror ships with key-only SSH and a fail2ban jail. Back it up, check it and
move its key with [node backup and recovery](operations/node-backup-and-recovery.md);
set storage limits and takedown handling with the
[node operator policy](operations/node-operator-policy.md). A mirror that should
work with no ForkMesh server at all can serve its own dashboard, SSH writes and
forks ([direct HTTPS gateway](operations/direct-https-mirror-gateway.md)).

## Instance

Start one at [forkmesh.com/start](https://forkmesh.com/start): fill in the page,
upload a [settings file](start-config.md), or let a
[coding agent](coding-agents.md) write the settings for you. ForkMesh connects
to your server over SSH, checks the host key and runs the installer while the
page shows each step. Your SSH password or key is used once and never stored.
Without a domain the instance gets a free `<ip>.sslip.io` address. An instance
nobody signs in to within five minutes removes itself.

After setup you control legal pages, privacy mode (no visitor logs),
moderation and whether to join the forkmesh.com directory:
[instance legal pages](app/instance-legal-pages.md),
[privacy mode](app/privacy-mode.md),
[instance federation](app/instance-federation.md). Operator detail:
[docs/operations/instances.md](operations/instances.md).

## Fleet

forkmesh.com itself runs as three core nodes and a runner tier
([architecture](architecture.md)). The same tooling (`tools/next_fleet.py`,
`tools/next_deploy.py`, `tools/next_dns.py`, `tools/next_vault.py`,
`tools/next_backup.py`) builds and runs any fleet described by a
`deploy/fleet.toml`. Start with [docs/operations/README.md](operations/README.md),
then [docs/operations/served-stack.md](operations/served-stack.md), [docs/operations/vault.md](operations/vault.md) and
[docs/operations/runners.md](operations/runners.md).

## Keep it running

- Recovery from losing a server, domain, key or backup:
  [disaster recovery](disaster-recovery/README.md).
- Identity export and recovery: [portable identities](portable-identities.md).
- Private repositories and backups:
  [private repository confidentiality](security/private-repository-confidentiality.md).

Related: [mirroring and distribution](mirroring.md),
[censorship resilience](censorship-resilience.md), public index
[README.md](README.md).
