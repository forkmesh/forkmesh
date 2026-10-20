# Git SSH public-key authentication

ForkMesh serves standard `ssh://` clone and push URLs from an independently
operated OpenSSH origin; the HTTP control plane never terminates SSH. There are
two planes:

1. **Control plane.** Stores each user's normalized public key in an encrypted
   envelope indexed by a keyed blind index, and enforces repository owner and
   organization-team permissions through `POST /api/ssh/authorize`
   (`lookup` during key authentication, `authorize` per Git operation).
2. **SSH origin.** [`tools/ssh_gateway.py`](../../../tools/ssh_gateway.py) runs as
   `AuthorizedKeysCommand` and as a forced command that executes only
   `git-upload-pack` or `git-receive-pack` against an exact local allowlist.

Private keys never leave the user's SSH agent or filesystem.

## Keys and permissions

Users register one OpenSSH public-key line at `/dashboard/settings/ssh-keys`.
Accepted: Ed25519 (including FIDO), ECDSA P-256/384/521 (including FIDO P-256),
RSA 3072–16384 bits. DSA, short RSA, key options, multiple lines and duplicate
or revoked keys are rejected with a generic error. A key grants no write by
itself: `git-receive-pack` needs ownership or `write`/`maintain`/`admin` through
an organization. Revocation applies on the next authentication. Ordinary Git
semantics (force-with-lease, force pushes) work unchanged; repository hooks are
disabled. Branch protection comes from an `operatorPolicy` block in the gateway
config, enforced by a gateway-owned `pre-receive` hook; see
[Node operator policy](node-operator-policy.md#protected-branches).

## Control-plane configuration

```dotenv
SSH_GATEWAY_HOST=ssh.example.org
SSH_GATEWAY_PORT=22
SSH_GATEWAY_TOKEN=<32-512 base64url characters>
SSH_GATEWAY_REPOSITORIES=owner/widget=read-write,owner/docs=read-only
```

No SSH URL is published unless all four are valid, and only for listed
repositories (`*` lists every repository; see [Relay mode](#relay-mode-self-hosted-instances)). The token authenticates the gateway's control-plane calls only.
`SSH_GATEWAY_REPOSITORIES` must equal the gateway's
`ssh-gateway-entrypoint worker-allowlist` output; set it to `disabled` to stop
publication.

## Node-side boundary

Four non-root accounts separate duties:

| Account | Role |
| --- | --- |
| `forkmesh-ssh-auth` | Holds the bearer token, runs the [authorization broker](../../../tools/ssh_authorization_broker.py) on a Unix socket, checks `SO_PEERCRED` |
| `forkmesh-ssh-lookup` | `AuthorizedKeysCommand`; may only send `lookup` |
| `git` | Forced Git service; may only send `authorize`; cannot read the token or node keys |
| `forkmesh-mirror` | Owns the bare repositories' signing and encryption material; no SSH login |

Configuration: `/etc/forkmesh/ssh-gateway.json` from
[`ssh-gateway.json.example`](../../../server/packaging/ssh/ssh-gateway.json.example),
validated by [`ssh-gateway-config.schema.json`](../ssh-gateway-config.schema.json).
Its `limits` block bounds concurrent sessions and processes (with slots reserved
for authorized pushes), upload/receive deadlines, `receiveMaxInputBytes`,
per-repository `maxStorageBytes` and pack threads. The forced command parses
`SSH_ORIGINAL_COMMAND` without a shell, accepts only Git protocol v1/v2 and one
`<owner>/<repository>.git`, runs Git with a fixed argv and clean environment,
disables hooks, credential helpers and the `ext` transport, and kills the whole
process group on timeout. The `99-forkmesh-git.conf` sshd `Match` block denies
passwords, forwarding and TTYs for `git`. Storage limits are application limits;
use filesystem quotas for a hard boundary.

After a successful receive a fixed root-owned notifier writes a coalesced
marker; a systemd path unit runs the
[headless refresh](headless-mirror-refresh.md) as `forkmesh-mirror`, verifies
signed health, and registers the new generation, retrying with capped backoff
(see [`ssh-post-receive-refresh.schema.json`](../ssh-post-receive-refresh.schema.json)).
`forkmesh-mirror-reconcile.timer` reconciles every 30 minutes.

Validate before reloading: `ssh-gateway-entrypoint check`, `sshd -t`, then
`systemctl reload sshd` with an administrative session kept open.

## Relay mode (self-hosted instances)

An instance that hosts its repositories on its own core node can accept SSH
without giving the gateway any access to repository storage. With an
`httpGit` block in place of `repositoryRoot`/`repositories`, the gateway
authorizes each operation as usual, then relays the session byte for byte to
the instance's own HTTPS Git routes:

```json
"httpGit": {"origin": "https://git.example.org", "connectAddress": "127.0.0.1:443"}
```

- **Every repository, no list.** Set `SSH_GATEWAY_REPOSITORIES=*`: hosted
  repositories and new forks work over SSH the moment they exist. Each
  `authorize` still checks the key's account against the repository, and the
  relayed request is checked again by the HTTPS route.
- **One push path.** `authorize` returns a credential for that key, account,
  repository and operation, valid for 30 minutes and revoked with the key.
  The relayed push is an ordinary HTTPS push: the full ref advertisement (only
  new objects are sent), branch protection rule by rule, contributor pushes to
  `refs/heads/pr/<account>/` (at most 32 branches) on repositories listed in
  `HOSTED_GIT_CONTRIBUTOR_PUSH`, quotas, and the attestation and push
  bookkeeping recorded after an accepted push.
- **Protocol.** Fetch is relayed as protocol v2 (Git 2.26 or newer; older
  clients add `-c protocol.version=2`). A push's pack is parsed to find its
  end; packs over 1 MiB stream chunked. `connectAddress` keeps the traffic on
  the host, so a CDN's request-size limit does not apply.
- **Short addresses.** With short repository addresses on, `git@host:Repo.git`
  names the short-address owner's repository, as `/Repo` does on the web.

`tools/install_ssh_relay.sh --host ssh.example.org` installs the accounts,
the broker (`apiConnectAddress` set to the same loopback front end), the
configuration under `/etc/forkmesh-ssh`, the `SSH_GATEWAY_*` settings in
`app.env` (instance updates keep them), and the
[`60-forkmesh-git-relay.conf`](../../../server/packaging/ssh/60-forkmesh-git-relay.conf)
`Match User git` block, validated with `sshd -t` before a reload.
`--check` reports the state; `--uninstall` removes the drop-in, the broker
and the settings.

For users, once a key is registered in Settings:

```sh
ssh -T git@ssh.example.org                   # a greeting; there is no shell
git clone git@ssh.example.org:owner/repository.git
git push origin HEAD:pr/<account>/my-fix     # contributor branch, where allowed
```

## Controller-free operation

By default every lookup and authorization is an HTTPS call, so a control-plane
outage stops SSH. An independent mirror can authorize locally by adding
`localAuthorization` to the broker config:

```json
"localAuthorization": {
  "mode": "fallback",
  "grantsFile": "/etc/forkmesh/ssh-local-grants.json",
  "dynamicRepositoriesFile": "/var/lib/forkmesh-mirror/gateway/dynamic-repositories.json"
}
```

- `fallback` consults the grants file only when the control plane is
  unreachable, never after an authoritative denial.
- `local` never contacts the control plane; the grants file is the whole policy.

The grants file (`type: forkmesh.ssh-local-grants`) lists each key, its account
and per-repository `access`: `read-only`, `read-write` or `pull-request` (push
only to `refs/heads/pr/<account>/*`). Setting `pullSubmissionSpool` accepts
signed pull submissions via `forkmesh-pull-submit <owner>/<repo>` and replays
them later with `ssh_gateway.py spool-forward`. Setting
`dynamicRepositoriesPath` enables `forkmesh-fork <owner>/<repo>`.

## Transport and round-robin origins

Plain proxied DNS does not carry SSH: use a DNS-only record, a TCP proxy, or a
tunnel with a client `ProxyCommand`, on a hostname separate from HTTPS.
`tools/provision_ssh_gateway.sh <host-ip>` installs a read-only gateway
idempotently. Every member keeps its own host key and presents a certificate
from the host CA (`tools/sign_ssh_host_certs.sh`, CA key outside the repository
via `FORKMESH_SSH_HOST_CA`, one-year validity), so clients trust one CA line
in `known_hosts`:

```text
@cert-authority ssh.forkmesh.com,*.forkmesh.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII5OvM7x8w4o1auHqYiSObIWHaVdAy4tT8dxLOai76Vs
```

## Failure and privacy behavior

Unknown, revoked and wrong-account keys fail identically; missing and
unauthorized repositories give the same denial. The gateway never runs a shell,
logs no paths, IPs, key material or tokens, and the broker returns no account
principal. Account renames follow the key; account deletion removes its keys.
