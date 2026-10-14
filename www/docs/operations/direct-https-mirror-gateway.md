# Direct-HTTPS independent mirror gateway

`tools/mirror_gateway.py` is the repository-byte origin of an independently
operated ForkMesh mirror. It listens on loopback HTTP only; an
operator-controlled HTTPS reverse proxy terminates public TLS. Git clients use
the mirror's standard `/<owner>/<repository>.git` Smart-HTTP URL directly. The
main routing service keeps the stable clone URL and redirects there only after
a fresh signed mirror proof.

Repository bytes always travel over HTTPS. The node event WebSocket only
announces changes (and renews the endpoint lease); the node daemon then fetches
over HTTPS, with a slow reconcile interval as recovery for missed events.

The Git data plane is read-only: it implements `upload-pack`, not general
`receive-pack`, issue writes or administration. Writes are opt-in per
repository (`merge-pull`, `git-receive-pack`, or a writable root for
push-created repositories). Requests without valid signed device headers get
`401 push_unauthorized` from the routing service before any body is read
(see [mobile node contract](../mobile/mobile-node-contract.md) section 7).
Owner-authorized stock Git writes use the [SSH gateway](git-ssh-gateway.md);
the [stock Git compatibility matrix](../git-compatibility.md) lists the split.

## Security boundary

- The listener must be `127.0.0.1`, `::1` or `localhost`.
- Public upload-pack reads need no capability. Every control, mutation, private
  and non-Git request needs a fresh, single-use Ed25519 capability from the
  routing service, bound to method, path plus query, body digest, node, request
  ID and issue time.
- Unknown, private, disabled and integrity-quarantined repositories all return
  the same `404 {"ok":false,"error":"not_found"}`. Health and manifest
  responses never enumerate repositories.
- Only configured public repositories are served, each pinned by a SHA-256
  over its sorted heads and tags, checked at startup and before serving.
- Git runs without a shell and with config, prompts, helpers, fsmonitor and
  pack hooks disabled.
- `refs/forkmesh/` is internal node state. Upload-pack hides the namespace and
  keeps `allowTipSHA1InWant`, `allowReachableSHA1InWant` and
  `allowAnySHA1InWant` disabled so hidden tips cannot be fetched by object ID.
  The SSH gateway applies the same rule plus `receive.hideRefs`.
- Release files are content addressed and re-hashed before sending.
- Logs hold only time, request ID, operation, status, duration and byte count.
- The configuration accepts no key, token, password or credential field.

Schemas: configuration `www/docs/mirror-gateway-config.schema.json`; endpoint
manifest `www/docs/mirror-gateway-manifest.schema.json` (non-proxied public
A/AAAA origin, `mainProxyMode: direct`, `/{owner}/{repository}.git` template).
A node must prove its direct HTTPS endpoint before it is published.

## Identity helpers

The gateway delegates Ed25519 to two shell-free local commands, each reading one
bounded JSON object on stdin and writing one on stdout, with credential-like
environment variables removed. Neither response may contain a private-key field.
See [Headless mirror identity](headless-mirror-identity.md) for a VPS
implementation.

| Command | Input `type` | Output |
| --- | --- | --- |
| `healthSignerCommand` | `forkmesh.health-challenge-signing` (`publicKey`, `messageBase64`, `messageSha256`) | `publicKey`, `signature` |
| `requestVerifierCommand` | `forkmesh.request-capability-verification` (adds `signature`) | `valid`, `publicKey` |

## Configuration

```bash
python3 tools/mirror_gateway.py --refs-sha256 /srv/forkmesh/mirrors/example.git
python3 tools/mirror_gateway.py --config /etc/forkmesh/mirror-gateway.json --check
python3 tools/mirror_gateway.py --config /etc/forkmesh/mirror-gateway.json
```

Main keys: `schemaVersion`, `node` (`name`, `publicKey`), `routerPublicKey`,
`publicOrigin`, `listen` (`host`, `port`), `manifestPath`,
`requestVerifierCommand`, `healthSignerCommand`, `limits`, and `repositories[]`
with `owner`, `name`, `visibility`, `enabled`, `gitDir`, `releaseStore` and
`integrity.expectedRefsSha256`.

- An omitted `visibility` is private. A private entry cannot be enabled or
  carry a storage path; it is never inspected or announced.
- `gitDir` must be an absolute, real, service-owned, non-world-writable bare
  repository. Public data is not encrypted; public `encryptedArchive` entries
  are rejected.
- An omitted `releaseStore` falls back to `<gitDir>/forkmesh-releases` when that
  is a real directory.

## Independent mirror operation

A mirror on its own hostname can offer browsing, clone, push, fork and pull
requests without the control plane. Every capability is opt-in and additive,
and none changes repository identity: dashboard, SSH and mirror URLs are
replaceable metadata over the same canonical repositories.

### Same-codebase dashboard

The gateway can serve the same website as the main App, from the build outputs
(`app/public/**`, `www/public/**`) stored as Git blobs in a mirrored public
repository, so dashboard updates arrive through ordinary sync under the
integrity pin. `api-client.js` resolves its API origin to `location.origin` on
non-canonical hosts.

```json
"dashboard": {
  "enabled": true,
  "assetsRepository": "<owner>/<repo>",
  "canonicalOrigin": "https://app.example.com",
  "sshOrigin": "ssh://git@ssh.example.com",
  "replicaPath": "/var/lib/forkmesh-mirror/gateway/dashboard-replica.json"
}
```

- `assetsRepository` must be an enabled public repository in the same file.
- `canonicalOrigin` receives `/login` and `/signup` when the browser holds no
  valid portable session grant. A mirror never receives passwords, controller
  tokens or session rows.
- `sshOrigin` is shown as each repository's clone/push URL.
- `replicaPath` is a `0600` last-good bundle of the controller's allowlisted
  public projections (`app/src/mirror_replica.py`,
  `app/src/public_snapshots.py`). It is not a database copy; failed refreshes
  keep the prior valid documents, and responses from it carry
  `X-ForkMesh-Replica: public-projection`.

Added routes: static pages and assets, read-only `/api/repositories`,
`/api/version` and `/api/repo/<owner>/<repo>/<read-op>` (alias of
`/v1/public/repositories/...`), plus `GET /api/accounts/sessions` and
`POST /api/accounts/session-handoff`, which accept only a controller-signed,
expiry-bounded grant and keep it in an HttpOnly cookie. Rendered documents are
held in bounded LRU caches invalidated on generation change (SIGHUP, or SIGUSR1
for the replica), never by time; large bodies stream.

### SSH writes without the control plane

`tools/ssh_gateway.py` and `tools/ssh_authorization_broker.py`
([git-ssh-gateway.md](git-ssh-gateway.md)) normally authorize through the
routing service. `localAuthorization.mode` adds `fallback` (a signed local
grants file consulted only when the service is unreachable, never after a
denial) or `local` (the grants file is the whole policy). Grants map a key to an
account and per-repository `read-only`, `read-write` or `pull-request` access;
`pull-request` may push only to `refs/heads/pr/<account>/*`.

### Contributor pull requests over SSH

With `pullSubmissionSpool` set, the forced command
`forkmesh-pull-submit <owner>/<repo>` accepts a signed pull request or review
comment on stdin, verifies its Ed25519 signature (`forkmesh-pull-event-v1` /
`forkmesh-pull-comment-v1`; the author field is the public key) and spools it
with per-key and global caps. A timer replays the spool to the controller:

```bash
python3 tools/ssh_gateway.py --config … spool-forward --api-origin https://app.example.com
```

Delivered items move to `forwarded/`, permanent failures to `rejected/`.

### Forking over SSH

With `dynamicRepositoriesPath` set (and the broker's
`localAuthorization.dynamicRepositoriesFile` pointing at the same file),
`forkmesh-fork <owner>/<repo>` clones heads and tags into a new bare repository
for the caller, registers it and refreshes the gateway. Global catalog
publication still goes through the owner's signed path.

### Push-created repositories

Set in the refresh configuration (`www/docs/headless-mirror-refresh.schema.json`),
never by hand in the generated gateway file:

| Key | Rule |
| --- | --- |
| `writableRepositoryRoot` | absolute, real, mode-0700 directory owned by the mirror account |
| `writableOwners` | unique owner names (max 64), or exactly `["*"]`; removing an owner stops reads and pushes from the next refresh |
| `receiveMaxInputBytes` | optional, default 256 MiB; becomes `receive.maxInputSize` |

On a capability-bound push the gateway creates
`<writableRepositoryRoot>/<owner>/<repo>.git`, runs `git receive-pack` with
deletes, non-fast-forwards and hooks refused, records it in
`hosted-pushes.json` (treated as untrusted input) and touches
`/run/forkmesh-mirror-refresh/pending` to trigger a refresh. At most two pushes
run at once (`503 push_busy`). The pushing device, not the node, publishes the
catalog record. These repositories are exempt from the immutable refs pin;
their generation is re-proved on every signed health check.

## Private replicas

Private repositories are never materialized by the gateway. They are stored
as opaque `.fm-private` files (`desktop/src/PrivateMirrorStore.cpp`): a random
id, an AES-256-GCM archive, hybrid X25519 + ML-KEM-768 key wraps per recipient
and a key epoch, with no names or paths. Rotation re-seals to the exact
recipient set. The gateway streams `/v1/private-replicas/<opaque-id>` only
under a routing capability and cannot decrypt it.

## Routing service contract

The routing service owns authorization, mirror selection and bounded failover.
Its database stores discovery and health metadata only. Generate its capability
key pair for `app/.env.production`:

```bash
python3 -c 'import base64;from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey as K;from cryptography.hazmat.primitives import serialization as s
k=K.generate();e=lambda b:base64.urlsafe_b64encode(b).decode().rstrip("=")
print("MIRROR_ROUTER_PUBLIC_KEY="+e(k.public_key().public_bytes(s.Encoding.Raw,s.PublicFormat.Raw)))
print("MIRROR_ROUTER_SIGNING_SEED="+e(k.private_bytes(s.Encoding.Raw,s.PrivateFormat.Raw,s.NoEncryption())))'
```

Keep the seed in the operator's secret store only; mirrors get the public key.
`deploy.sh secrets` preserves it; `deploy.sh rotate-mirror-router` changes it.

1. **Registration.** `POST /api/mirrors/https` (`node`, `baseUrl`,
   `mirrorUrl`, `publicKey`, `issuedAt`, `signature` over
   `forkmesh-https-endpoint-v2`). The service verifies the signed
   `/forkmesh-mirror.json` manifest, the SSRF policy and DNS.
2. **Signed health.** `GET /health?nonce=…&issuedAt=…[&owner=…&repo=…]`
   (`forkmesh-https-health-v1` / `forkmesh-https-health-repository-v1`)
   returns `available`, `integrity`, `refsSha256` and `operationsSha256`.
   Refs are checked against the owner-signed state; health is valid for two
   minutes.
3. **Capability.** `forkmesh-masked-proxy-v1` signature in `X-ForkMesh-Node`,
   `X-ForkMesh-Request-Id`, `X-ForkMesh-Issued-At`, `X-ForkMesh-Body-Sha256`
   and `X-ForkMesh-Signature`; request IDs are single-use for 60 seconds.
4. **Selection.** Upload-pack is redirected to a `mirrorUrl` only after a fresh
   proof there; other reads are proxied. Failover happens only before response
   bytes are sent; cookies, auth headers and client IPs are never forwarded.

## Operations

`/v1/repositories/<owner>/<repo>/<operation>` requires a capability.
`/v1/public/repositories/...` serves the GET/HEAD read operations of public
repositories without one.

| Operation | Method | Query | Response |
| --- | --- | --- | --- |
| `git-info-refs` | GET/HEAD | `service=git-upload-pack` | upload-pack advertisement |
| `git-upload-pack` | POST | none | streamed pack (gzip allowed, 8 MiB request bound) |
| `merge-pull` | POST | typed JSON | asynchronous exact-OID merge via a configured executor |
| `update-pull-branch` | POST | typed JSON | merge base into a pull head |
| `actions-status` | GET | none | redacted run summaries for the repository |
| `actions-control` | POST | none; typed JSON ≤ 4 KiB | `202` once queued for the executor |
| `tree` / `blob` / `blobs` / `raw` | GET/HEAD | `path`, `ref` | bounded JSON or streamed bytes |
| `history` / `commit` / `branches` | GET/HEAD | `ref` / `path` | commit metadata |
| `search` / `stats` / `sizes` | GET/HEAD | `path`, `ref` | bounded results |
| `release-blob` | GET/HEAD | `sha256` | verified release bytes |

`actions-status` (`actionsSummaryPath`) and `actions-control`
(`actionsControlPath`, mode 0700) are off by default; the gateway is only a
courier and the executor re-verifies each request. See
[Mirror Actions control and summary v2](mirror-actions-control.md) and
[`mirror-actions-control.schema.json`](../mirror-actions-control.schema.json).
Upload-pack supports protocol v2 filters (`--filter=blob:none`) and shallow
clones.

## Cloudflare Tunnel fallback

For nodes without a reachable origin, `tools/cloudflare_tunnel_bootstrap.py`
provisions a Tunnel to `http://127.0.0.1:<port>` and a signed manifest with
`mainProxyMode: masked`. It reads the API token from `CLOUDFLARE_API_TOKEN` or
stdin, supports `--dry-run`, and needs `--replace-dns` to overwrite DNS.
