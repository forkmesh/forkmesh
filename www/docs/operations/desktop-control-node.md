# ForkMesh desktop control node

The Qt desktop client includes a **Control** page in its main navigation. It is
the operator surface for the current device; it does not turn ForkMesh into a
custodian.

The page is a row of tabs, in this order: Site deployment (**Deploy**), Export
system (**Export**), Import and deploy (**Import**), Mirror client, Repository
permissions, Keys and wallet, Reward pool, Cloudflare relay, and API token
(`desktop/src/MainWindowControlNode.cpp`). Each tab scrolls on its own. The page
title and the privacy note stay pinned above the tab bar. Host management is not
a Control tab; it lives under **Network → Hosts** (see
[Connected hosts](#connected-hosts)).

## Mirror client operations

The status card shows the relay connection, tracked local mirror paths,
repositories configured for managed nodes, and bounded sync jobs. The desktop
does not start a gateway, host repositories, publish catalog rows, or register
mirror endpoints.

- **Sync all mirrors** runs the same asynchronous mirror fetch path used by the
  regular repository UI.
- **Run health check** verifies the local Ed25519 identity, Git executable,
  Cloudflare bootstrap source, mirror paths, and relay connection.
- **Open full local log** opens the existing durable device log. Cloudflare
  process output stays in the Control page's bounded, in-memory output pane.

The permissions table controls each local repository:

- **Private** controls public discovery and authorized browse/clone access.
- **Serve** records whether managed mirror hosts may publish and serve the
  repository; the desktop itself never becomes a serving host.
- **Run actions** controls whether trusted `.forkmesh` workflows may run locally.
- **Secret scan** controls push protection for high-confidence credentials.

Only a device holding the repository owner's signing identity can change
visibility or network-serving permission. Permission changes are recorded in the
local audit log. Private repository names and contents are not sent by the
control page to unrelated services.

## Identity and wallet

ForkMesh's Ed25519 identity key is generated and used on the local device with
owner-only filesystem permissions. **Back up or import identity key** reuses the
existing passphrase-encrypted keyfile workflow.

The wallet field accepts only a base58 Solana public address that decodes to 32
bytes. It rejects seed phrases, recovery phrases, and private-key material.
ForkMesh stores the public payout address only:

- ForkMesh does not create or hold a wallet for the user.
- Wallet private keys remain in the user's external self-custodial wallet.
- Connecting an address does not authorize a transfer.
- Displayed balances and visual rewards are not custody or guaranteed returns.

## Cloudflare one-click deployment

The **Deploy** tab begins with two portable recovery/deployment actions (the
**Export** and **Import** tabs hold the same flows):

- **Export complete system…** writes one indented
  `forkmesh-complete-system-v1` JSON file. It includes every typed `QSettings`
  value, every Actions variable/secret, the native settings file,
  the Worker's `app/.env.production` (the directory is found from the Cloudflare
  bootstrap script by `siteDeployEnvFilePath()` in `desktop/src/ControlNode.cpp`),
  and every file and empty directory under
  ForkMesh application data—including the node identity, SSH keys, account,
  histories, and local stores. Session-only Cloudflare/Vultr credentials, host
  passwords, and available Claude/Codex login payloads are included explicitly.
  Each embedded file carries its size, mode, and SHA-256 digest. The export is
  committed owner-only and fails instead of following a symbolic link. It does
  not copy the installed application binary or working copies stored outside
  application data; their configured paths remain in settings.
- **New deployment…** opens a compact four-column screen: bundle, credentials,
  instance/network, and review/deploy. Import validates the schema, every path,
  base64 payload, size, and digest before showing any values. **Deploy
  instance** applies the bundle's typed settings and complete variable set,
  overlays any reviewed Vultr/Cloudflare edits, discards stale in-progress
  deployment checkpoints, and hands the request to the existing durable
  six-stage Vultr provisioner.

The JSON intentionally contains private keys and secrets in clear text so it
can reconstruct a controller from scratch. Anyone holding it can act as that
node. Keep it in encrypted storage and delete unneeded copies.

The deployment card invokes ForkMesh's pinned `tools/cloudflare_bootstrap.py`.
A source build uses the checkout; CMake installs and native desktop packages
carry the same `tools/`, `app/`, `www/`, and `world/` resource trees, including Worker
source, generated/static assets, migrations, and the pinned Wrangler wrapper.
The release flow therefore does not depend on the build machine's source path.

1. For a token scoped to one account and one active zone, leave the topology
   fields empty and paste the least-privilege Cloudflare API token. ForkMesh
   discovers the zone and derives distinct relay and direct-mirror hostnames.
2. If the token can see more than one account or zone, select the account,
   zone, and hostnames in the advanced fields; automatic setup fails closed
   instead of guessing.
3. Use **Validate Worker + mirror** for a read-only dry run, or
   **Deploy Worker + mirror** to
   create/reuse D1, deploy the Worker, configure proxied DNS and the Worker
   route, provision the direct Tunnel, and run health checks.
4. When **Add and connect** is enabled, the successful hostname is added to the
   local relay list and selected.

Credential boundaries:

- The token typed into this card is never written to `QSettings`. Storing a
  credential is only ever the explicit result of the API token tab's rotation
  (see below) or of Settings → Quick Setup.
- The token is never included in process arguments.
- The token is passed only as `CLOUDFLARE_API_TOKEN` in the local child process
  environment.
- The desktop never passes `--secret-env`, so the API token cannot become a
  Worker secret.
- Exact tokens and credential-shaped output are redacted before display.
- Only public deployment fields are saved locally for reuse.
- Generated temporary Wrangler configuration contains public resource IDs only;
  the bootstrapper removes it after the run.
- If the host has no `cloudflared`, the desktop invokes its bundled Python
  installer directly (never `curl | sh`). It downloads Cloudflare release
  `2026.7.2` over HTTPS into owner-only app data, accepts only the supported
  OS/architecture assets and trusted GitHub release hosts, verifies the exact
  repository-pinned SHA-256 archive and executable digests, and atomically
  installs the connector. A managed copy is reverified once per desktop
  process before it is executed.

Python 3 remains an explicit host prerequisite. The Worker deploy wrapper may
fetch its version-pinned `workers-py<1.18.0` and `wrangler@4.136.2` dependencies
on first use; those third-party runtimes are not embedded in ForkMesh.

The bootstrapper asks an external signer for the endpoint manifest. It launches
the same ForkMesh binary with `--sign-mirror-manifest`; that mode reads the
public canonical manifest on standard input, validates its type, target public
key, encoding, and SHA-256 digest, signs with the existing local Ed25519
identity, and emits only the public key and detached signature. The private key
never leaves the desktop process.

## API token

The **API token** tab answers two questions without leaving the desktop: what an
existing Cloudflare API token can do, and how to replace it with one scoped to
exactly what ForkMesh needs.

**Check token permissions** uses the token in the field, else the token in the
Cloudflare relay tab, else this device's saved `CLOUDFLARE_API_TOKEN` variable.
It calls `GET /user/tokens/verify`, then reads the token's own policy
(`GET /user/tokens/<id>`) when the token may read itself, and finally probes each
capability with one read-only request. The table reports, per permission, whether
ForkMesh requires it, whether the token's policy grants it, and what the live
probe answered. Two honest limits are stated on the page: a token without
**API Tokens: Read** cannot report its own policy, and a read-only probe can
never prove *edit* rights — only a deploy does.

Required permissions:

| Cloudflare permission | Used for |
| --- | --- |
| Account Settings: Read | discovering the account that owns the Worker, D1 and Tunnel |
| Workers Scripts: Edit | uploading the Worker, its assets, Durable Objects, cron triggers and secrets |
| D1: Edit | creating and migrating the `forkmesh` database |
| Zone: Read | finding the zone the relay/mirror hostnames derive from |
| DNS: Edit | the proxied relay and mirror records, plus each one-click mirror's A record |
| Cloudflare Tunnel: Edit | the direct-HTTPS mirror gateway's Tunnel and connector credential |

Optional: **Workers Tail: Read** (the live Worker log in Network → Logs), and
**API Tokens: Read** / **API Tokens: Edit**, which this tab itself needs to
report an exact policy and to mint a replacement.

**Generate and install a new token** requires a current token with API Tokens:
Edit, and an unambiguous account and zone (run the check first; set them on the
Cloudflare relay tab if they stay unknown). It resolves permission-group ids from
the account's own `GET /user/tokens/permission_groups` — no id is hardcoded —
mints one token scoped to that account, that zone and this user, and then:

1. replaces `CLOUDFLARE_API_TOKEN` in this device's variables (the value every
   Actions run and the deploy workflow read), adding `CLOUDFLARE_ACCOUNT_ID` when
   it is known and unset;
2. replaces `CLOUDFLARE_API_TOKEN` in `app/.env.production` in
   place, keeping every other production secret, comment and ordering, dropping a
   later stale duplicate of the same name, and writing the file owner-only;
3. re-runs the permission check against the new token.

The minted token is never displayed: the page names it by its last four
characters only, and it is redacted from the tab's output pane. The token used to
mint it stays valid until it is deleted in the Cloudflare dashboard, so an
operator can roll back by pasting the old value back into the field.

## Connected hosts

**Network → Hosts** is where operators add a remote machine, install, update or
remove ForkMesh, sign in the host's agent CLIs, and view host logs; it also
offers a confirmed fleet action to upload the current binary to saved hosts.
Every successful host update also refreshes the Claude Code and Codex CLIs the
host serves to its `forkmesh-node` service (root only, never fatal), so a mirror
does not keep a CLI too old for new models. See
[docs/agents/README.md](../../../docs/agents/README.md#agent-cli-refresh-on-hosts).

Every host in the fleet authorizes one shared ForkMesh SSH key. The desktop
keeps its private half owner-only in its SSH directory, at
`~/projects/forkmesh/ssh/forkmesh_shared_ed25519` in the default layout
(`sharedHostKeyPath()` in `desktop/src/ControlNode.cpp`) — a device set up before the switch
keeps using the per-provider file it already has (`vultr_mirror_ed25519`), so
mirrors provisioned earlier stay reachable without re-keying. The key is
generated once, on the first flow that needs it, and registered with the
provider as `forkmesh-shared-host-key` when a mirror is provisioned.

Every host operation — install, update, uninstall, logs, size map, Actions, the
agent-CLI installer and the sign-in shell — authenticates with that key, whether
the host was provisioned by ForkMesh or added by hand in **Add a host**. A host
saved without its own recorded key falls back to the shared one, so the password
field stays optional: leave it blank when the host already carries the shared
key.

SSH passwords remain on the desktop and are supplied to local SSH tooling via
environment/stdin, not command-line arguments. They are never sent to the
Cloudflare Worker. A password is only the fallback ssh reaches after the shared
key is refused; the size map still asks for one when no key or password works.
Prefer dedicated hosts and scoped accounts.

## First-instance reward-pool signer

The Control page also contains the explicit local signer for the public
community reward pool. It imports an existing Solana key into a
passphrase-encrypted, owner-only local vault; it never generates or uploads that
key. The desktop verifies every Worker-authored transfer, shows the exact
recipients and amounts for confirmation, signs locally, submits directly to a
configured public HTTPS Solana RPC, waits for finality, and reports only the
public transaction signature to the Worker.

See [Community reward-pool local signer](community-reward-pool-signer.md) for
the vault format, configuration, exact request canonicalization, fail-closed
validation, recovery behavior, and production checklist.

## ForkMesh World

The desktop has no World entry in its rail or on the Control page. Open the
World in a browser at `https://world.forkmesh.com/`. Repository HTTP traffic
uses ordinary HTTPS; the World does not route clone or file traffic through
multiplayer sockets.
