# ForkMesh desktop control node

The desktop client's **Control** page is the operator surface for the current
device. It never makes ForkMesh a custodian. Its tabs are Site deployment,
Export, Import, Mirror client, Repository permissions, Keys and wallet, Reward
pool, Cloudflare relay, and API token (`desktop/src/MainWindowControlNode.cpp`).
Host management lives under **Network → Hosts**.

## Mirror client and permissions

The status card shows the relay connection, tracked local mirrors, managed-node
repositories, and sync jobs. The desktop itself never serves repositories or
registers endpoints. **Sync all mirrors** fetches every mirror; **Run health
check** verifies the local identity, Git, bootstrap tooling, mirror paths, and
relay connection.

Per-repository permissions: **Private** (discovery and authorized access),
**Serve** (whether managed hosts may publish it), **Run actions** (local
`.forkmesh` workflows), and **Secret scan** (push protection). Only a device
holding the owner's signing identity can change visibility or serving, and
every change is recorded in the local audit log.

## Identity and wallet

The Ed25519 identity key is generated locally with owner-only permissions and
can be backed up as a passphrase-encrypted keyfile. The wallet field accepts
only a 32-byte base58 Solana public address and rejects seed phrases and
private keys. Connecting an address authorizes nothing; private keys stay in
the user's own wallet.

## Export, import, and deployment

- **Export complete system…** writes one `forkmesh-complete-system-v1` JSON
  file with every setting, Actions variable and secret, the deployment
  environment file, and all application data (identity, SSH keys, stores),
  each with size, mode, and SHA-256. It is written owner-only and refuses
  symlinks. It contains private keys in clear text: store it encrypted.
- **New deployment…** validates a bundle's schema, paths, sizes, and digests
  before showing values, then applies it and hands off to the durable host
  provisioner.

Deployment runs the bundled `tools/cloudflare_bootstrap.py`. With a token
scoped to one account and zone it discovers the topology; with several it
requires explicit selection rather than guessing. **Validate** is a read-only
dry run; **Deploy** creates or reuses the database, deploys the server,
configures DNS and routes, provisions the tunnel, and runs health checks.

Credential rules: the token is never saved by the deploy card, never placed in
process arguments, passed only as `CLOUDFLARE_API_TOKEN` in the child
environment, never turned into a server secret, and redacted from output. A
missing `cloudflared` is installed by a bundled installer that verifies pinned
SHA-256 digests. Endpoint manifests are signed by launching the desktop binary
with `--sign-mirror-manifest`, which validates the manifest and returns only the
public key and signature.

## API token

**Check token permissions** verifies a Cloudflare token, reads its own policy
when allowed, and probes each capability read-only (a read probe cannot prove
edit rights).

| Cloudflare permission | Used for |
| --- | --- |
| Account Settings: Read | discovering the account |
| Workers Scripts: Edit | uploading the server, assets, triggers, and secrets |
| D1: Edit | creating and migrating the database |
| Zone: Read | finding the zone |
| DNS: Edit | relay and mirror records |
| Cloudflare Tunnel: Edit | the mirror gateway tunnel |

Optional: Workers Tail: Read, API Tokens: Read/Edit. **Generate and install a
new token** mints a replacement scoped to the resolved account, zone, and user,
replaces `CLOUDFLARE_API_TOKEN` in the device variables and deployment
environment file (owner-only, other entries preserved), and re-checks it. The
new token is shown only by its last four characters; the old one stays valid
until deleted, allowing rollback.

## Connected hosts

**Network → Hosts** adds remote machines, installs, updates, or removes
ForkMesh, signs in agent CLIs, and shows logs. Every host operation
authenticates with one shared fleet SSH key generated on first use and kept
owner-only on the desktop. Passwords are an optional fallback, stay on the
desktop, and are passed via environment or stdin, never arguments.

## Reward-pool signer

The Control page hosts the local signer for the community reward pool; see
[Community reward-pool local signer](community-reward-pool-signer.md).
