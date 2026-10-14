# Headless mirror identity

`tools/headless_mirror_identity.py` is the local cryptographic boundary for a
non-GUI mirror host. It runs as the same dedicated, unprivileged account as
`mirror_gateway.py` and:

- keeps one Ed25519 node identity and one age identity in an owner-only
  directory;
- signs only the bounded manifest, health, reclaim, registration, and public
  catalog-v2/state protocols, building each canonical message itself (there is
  no arbitrary-message signing);
- verifies routing capabilities only with the configured router public key.

It takes no private key, seed, token, or password on the command line. Requests
are bounded JSON on standard input; responses hold only public keys,
signatures, hashes, and public records.

Requirements: Python 3.12+, `cryptography`, `age`/`age-keygen`, and Git. The
identity directory must be an absolute path with mode `0700` (files `0600`),
outside any checkout, image, or shared repository.

## Initialize and configure

```bash
printf '%s\n' '{
  "schemaVersion": 1,
  "type": "forkmesh.headless-mirror-identity-init",
  "nodeName": "mirror-two",
  "routerPublicKey": "<MIRROR_ROUTER_PUBLIC_KEY>",
  "allowedOrigins": ["https://mirror.example.org"]
}' | python3 tools/headless_mirror_identity.py \
  --state-dir /var/lib/forkmesh/identity init
```

All values are public. `allowedOrigins` pins which hostnames the node key will
sign for. `init` is idempotent and reuses a valid existing key after an
interruption. `public-info` prints the public half. `configure`
(`forkmesh.headless-mirror-identity-configure`) changes only the router key and
origin pins, never the private identities.

## Gateway integration

Reference the helper from the gateway configuration as direct command arrays,
never through a shell:

```json
{
  "requestVerifierCommand": ["/usr/bin/python3", "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir", "/var/lib/forkmesh/identity", "capability-verify"],
  "healthSignerCommand": ["/usr/bin/python3", "/opt/forkmesh/tools/headless_mirror_identity.py",
    "--state-dir", "/var/lib/forkmesh/identity", "health-sign"]
}
```

Public repositories are served straight from the bare repository with an
`integrity.expectedRefsSha256` pin; the age seal/materialize modes exist only
for upgrade compatibility. `manifest-sign` signs endpoint manifests for a
tunnel or public HTTPS origin, using `nodePublicKey` from `public-info`.

After each push, the refresh orchestrator computes the heads-and-tags pin and
atomically installs a gateway configuration pointing at the bare repository; it
skips archives and full fsck, which the periodic `check` command runs.

Run the service with a restrictive umask, no privilege escalation, a read-only
application tree, and write access only to identity, repository, and gateway
state. The gateway listens only on loopback behind a TLS proxy or tunnel.

## Operator-only signatures

These modes are for local operator use only, never exposed over HTTP:

- `sign-reclaim` (`forkmesh.account-reclaim-signing`: `nodeName`, six-digit
  `linkCode`) returns the `/api/accounts/reclaim-node` body.
- `sign-endpoint-registration` (`forkmesh.https-endpoint-registration-signing`:
  `node`, `baseUrl`) returns the `/api/mirrors/https` body.
- `sign-catalog-v2` (`forkmesh.catalog-v2-publication-signing`: a public
  `record` with `owner`, `name`, `visibility: "public"`, and `stateHash`)
  returns the record with `stateSig` and `catalogSig` for `/api/repositories`.

## Key handling and rotation

Back up the identity directory through an encrypted, owner-controlled process;
it cannot be recovered. File permissions do not protect against root or a
compromised service account; use an encrypted volume or hardware signer when
that matters. To rotate the router key, update the server first, then run
`configure` on each mirror. To rotate the node identity, provision a new state
directory, reclaim the node, republish manifests and catalog state, verify
health, and retire the old directory.
