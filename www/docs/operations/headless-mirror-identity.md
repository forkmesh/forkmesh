# Headless mirror identity

`tools/headless_mirror_identity.py` provides the local cryptographic boundary
for a non-GUI mirror host. It is intended to run as the same dedicated Unix
account as `mirror_gateway.py`.

The helper:

- keeps one Ed25519 node identity and one native age identity in an owner-only
  directory;
- signs only the bounded ForkMesh manifest, health, reclaim, registration, and
  public catalog-v2/state protocols;
- verifies routing capabilities only with the configured router public key;
- snapshots a bare repository through Git and atomically seals exactly one
  `repository.git` tree with age; and
- authenticates and validates an age-encrypted tar before materializing it.

It has no command-line option for a private key, seed, token, password, or
credential. Requests use bounded JSON on standard input. Responses contain only
public keys, signatures, key references, hashes, sizes, and public records.

## Requirements

- Python 3.12 or newer
- `cryptography`
- the official `age` and `age-keygen` programs
- Git

Run the helper and gateway as a dedicated, unprivileged service account. Put
the identity directory and encrypted archives on storage owned by that account.
The identity directory and archive directory must be mode `0700`; identity
files are created mode `0600`.

```bash
install -d -m 0700 /var/lib/forkmesh/identity
install -d -m 0700 /var/lib/forkmesh/archives
```

The paths above are examples. They must be absolute. Do not put the identity
directory in the checkout, a shared Git repository, a container image, or a
configuration-management response.

## Initialize

All values in the initialization request are public. The allowed origin pin
prevents a local bootstrap process from asking the node key to sign a manifest
or registration for another hostname.

```bash
printf '%s\n' '{
  "schemaVersion": 1,
  "type": "forkmesh.headless-mirror-identity-init",
  "nodeName": "mirror-two",
  "routerPublicKey": "<MIRROR_ROUTER_PUBLIC_KEY>",
  "allowedOrigins": ["https://mirror9.forkmesh.com"]
}' | python3 tools/headless_mirror_identity.py \
  --state-dir /var/lib/forkmesh/identity init
```

Initialization is idempotent. If a process interruption leaves a valid
`node-ed25519.pem` or `age-identity.txt` before `public.json` is committed,
rerun the same request. The helper reuses the valid identity; deleting those
files would unnecessarily rotate the node.

To inspect the public half:

```bash
python3 tools/headless_mirror_identity.py \
  --state-dir /var/lib/forkmesh/identity public-info
```

`configure` changes only the public router-key and origin pins. It never rotates
either private identity:

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.headless-mirror-identity-configure",
  "routerPublicKey": "<MIRROR_ROUTER_PUBLIC_KEY>",
  "allowedOrigins": ["https://mirror9.forkmesh.com"]
}
```

## Gateway command arrays

Use direct JSON command arrays. Do not place them behind a shell:

```json
{
  "requestVerifierCommand": [
    "/usr/bin/python3",
    "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir",
    "/var/lib/forkmesh/identity",
    "capability-verify"
  ],
  "healthSignerCommand": [
    "/usr/bin/python3",
    "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir",
    "/var/lib/forkmesh/identity",
    "health-sign"
  ]
}
```

Public repositories do not use the age helper. The gateway receives the
protected plaintext bare path directly:

```json
{
  "gitDir": "/srv/git/forkmesh.git",
  "integrity": {
    "expectedRefsSha256": "<canonical heads-and-tags SHA-256>"
  }
}
```

The identity helper remains responsible for capability, health, manifest, and
catalog signatures. Its older public seal/materialize modes exist only for
upgrade compatibility and are not called by refresh or gateway serving.

For a Tunnel endpoint, give `cloudflare_tunnel_bootstrap.py` this helper as its
manifest signer command and supply `nodePublicKey` from `public-info` as the
mirror public key:

```text
/usr/bin/python3 /opt/forkmesh/tools/headless_mirror_identity.py --state-dir /var/lib/forkmesh/identity manifest-sign
```

The signer accepts the existing strict Cloudflare Tunnel manifest, the
independent `public-https-origin` manifest, the legacy Worker-route manifest,
and a secret-free `cloudflare-proxied-origin` manifest. The deployed Worker must
support the selected variant; Tunnel remains available for hosts without a
public HTTPS origin.

For a service manager, set a restrictive umask, a dedicated `User`, no
privilege escalation, a read-only application tree, and write access only to
the identity, public bare repository, and gateway-state directories. The
gateway itself must continue listening only on loopback behind Cloudflare
Tunnel or an equivalently restricted TLS proxy.

## Initial sync and post-receive refresh

After Git updates the bare source, run the refresh orchestrator. It computes the
canonical heads-and-tags pin and atomically installs a gateway configuration
that points directly at `sourceRepository`:

```json
{
  "schemaVersion": 1,
  "sourceRepository": "/srv/git/forkmesh.git",
  "gitDir": "/srv/git/forkmesh.git"
}
```

The push-triggered path performs no tar, age, archive materialization, or full
Git fsck. It validates the bare repository, checks that refs stay stable during
configuration validation, atomically swaps the refs-pinned config, and removes
legacy public `.age` generations. The explicit `check` command retains the full
Git fsck for periodic integrity monitoring.

## Operator-only signatures

The following modes are for a local operator workflow. Do not expose them as
HTTP endpoints or include them in the gateway configuration.

- `sign-reclaim` accepts
  `forkmesh.account-reclaim-signing` with `nodeName` and a six-digit
  `linkCode`; it generates the current timestamp and returns the exact public
  `/api/accounts/reclaim-node` body.
- `sign-endpoint-registration` accepts
  `forkmesh.https-endpoint-registration-signing` with `node` and `baseUrl`;
  it generates the current timestamp and returns the exact
  `/api/mirrors/https` body.
- `sign-catalog-v2` accepts
  `forkmesh.catalog-v2-publication-signing` with a public `record`. The record
  must include `owner`, `name`, `visibility: "public"`, and the
  `expectedRefsSha256` value as `stateHash`. It returns the normalized public
  record with `stateSig`, `catalogSigVersion: 2`, and `catalogSig`, ready for
  `/api/repositories`.

These modes construct the canonical messages internally; there is no
arbitrary-message signing mode.

## Key handling and rotation

Back up the identity directory through an encrypted, owner-controlled process.
ForkMesh does not upload it and cannot recover it. Filesystem permissions
protect the stopped host from other local accounts, not from root or a
compromised service account; use an encrypted volume or hardware-backed signer
when that threat is in scope.

To rotate the routing key, update the Worker secret first as a coordinated
maintenance action, then use `configure` on each mirror. To rotate the node or
age identity, provision a new state directory, rebind/reclaim the node identity,
reseal every archive, publish new manifests and catalog state, validate health,
and retire the old directory according to the operator retention policy.
