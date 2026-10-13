# Git SSH public-key authentication

ForkMesh supports familiar SSH clone and push URLs without pretending that a
Cloudflare HTTP Worker can terminate raw SSH. The implementation has two
separate planes:

1. The Worker stores each user's normalized **public** key in an encrypted D1
   envelope, indexes it by a keyed blind index, and enforces the existing
   repository owner and organization-team permissions.
2. An independently operated OpenSSH origin runs
   [`tools/ssh_gateway.py`](../../../tools/ssh_gateway.py) as both
   `AuthorizedKeysCommand` and a forced command. It executes only
   `git-upload-pack` or `git-receive-pack` against an exact local bare-repository
   allowlist after the Worker authorizes that key, repository, and operation.

Private keys and passphrases stay in the user's SSH agent or filesystem.
ForkMesh never asks for, uploads, derives, or stores them.

## User flow

An authenticated user opens `/dashboard/settings/ssh-keys`, pastes one OpenSSH
public-key line, and gives it a private display label. Supported key types are:

- Ed25519 and FIDO/U2F-backed Ed25519
- ECDSA P-256, P-384, P-521, and FIDO/U2F-backed P-256
- RSA with a modulus from 3072 through 16384 bits

DSA, undersized RSA, authorized-keys options, multiple lines, malformed wire
blobs, and a key already attached (or previously revoked) are rejected. The
settings page lists labels, algorithms, SHA-256 fingerprints, creation dates,
and last-use times; it never returns the stored public-key line after
registration.

Pasting a public key reserves that key to the signed-in account; the form itself
cannot prove possession of the private half. OpenSSH proves possession on every
connection before the forced command can run. Use a fresh, account-specific key
instead of reusing a publicly listed key. A globally duplicate key returns the
same generic conflict whether it is active or revoked, so the API does not
disclose which account registered it; rotate to a fresh key and contact the
account-support channel if a known key was reserved without permission.

When a gateway is configured, repository Code menus show:

```text
ssh://git@ssh.example.org/owner/repository.git
```

The same URL clones and pushes. Registering a key is not itself write
permission: every `git-receive-pack` is allowed only when the mapped account
owns the repository node or holds `write`, `maintain`, or `admin` permission
through a linked organization. Revocation takes effect on the next SSH
authentication/authorization check.

This is standard receive-pack behavior: branch refspecs, stale
`--force-with-lease` rejection, and explicit `--force` updates need no client
patch. Client-side hooks and rebase remain local Git operations. Repository
controlled server hooks are disabled at the gateway boundary; shared required
checks belong in reviewed Actions. Create and verify a Git bundle before an
intentional history rewrite. Exact commands and recovery steps are generated in
the [stock Git compatibility matrix](../git-compatibility.md).

To protect branches on an SSH origin, add `operatorPolicy` to the gateway
config:

```json
"operatorPolicy": {
  "policyPath": "/var/lib/forkmesh-ssh/operator-policy.json",
  "stateDirectory": "/var/lib/forkmesh-ssh/operator-policy"
}
```

Both paths belong to `git`, and the state directory is mode `0700`. On each
push, the gateway points `core.hooksPath` at `<stateDirectory>/hooks`, which it
owns and which holds only a `pre-receive` hook. It also passes the authorized
`owner/repository` to that hook.

The hook applies the policy's `branches` rules: no deletion, no force update,
no stale expected old value, and optionally only commits signed by an allowed
SSH signer. It also applies the policy's push-size and content limits. It
refuses the whole push, with recovery steps, before any ref moves. If the
policy cannot be read, it refuses every push. `ssh_gateway.py check` reports
`operatorPolicyConfigured` and `operatorPolicyValid`.

`tools/provision_ssh_gateway.sh` installs `node_operator_policy.py` beside the
gateway. The policy format, audit log, and rollback commands are described in
[Node operator policy](node-operator-policy.md#protected-branches).

## Worker configuration

Create one random infrastructure bearer token:

```bash
python3 -c 'import secrets; print(secrets.token_urlsafe(48))'
```

Configure these Worker secrets/variables through `.env.production` or the
one-click deployment variables:

```dotenv
SSH_GATEWAY_HOST=ssh.example.org
SSH_GATEWAY_PORT=22
SSH_GATEWAY_TOKEN=<random token>
SSH_GATEWAY_REPOSITORIES=alice-node/widget=read-write,alice-node/docs=read-only
```

`SSH_GATEWAY_HOST` is a bare hostname or IP address, never a URL.
`SSH_GATEWAY_PORT` is 1–65535. `SSH_GATEWAY_TOKEN` must contain 32–512
base64url characters (`A-Z`, `a-z`, `0-9`, `_`, or `-`).
`SSH_GATEWAY_REPOSITORIES` is the exact Worker-side copy of the
gateway's local allowlist, using `owner/repo=read-write` or
`owner/repo=read-only` entries. ForkMesh publishes no SSH URL unless all four
values are valid, and publishes a URL only for an explicitly listed repository.
The bearer token authenticates only the gateway's HTTPS control-plane calls; it
is not a user key and grants no repository permission by itself.

The gateway calls:

- `POST /api/ssh/authorize`, action `lookup`, during public-key authentication.
- `POST /api/ssh/authorize`, action `authorize`, for the exact
  upload-pack/receive-pack request.

The endpoint is bearer-gated, fail-closed, and returns no repository data.
Public-key registration and revocation, plus successful or denied push
authorizations, produce metadata-only sensitive-action audit entries.

## Node-side configuration

The production boundary uses four distinct non-root identities:

- `forkmesh-ssh-auth` owns the Worker bearer and runs the local authorization
  broker. It cannot read repositories or mirror identity material.
- `forkmesh-ssh-lookup` is used only by `AuthorizedKeysCommand`. It may submit
  `lookup` requests but cannot authorize repositories.
- `git` runs the forced Git service. It may submit `authorize`
  requests and write the selected bare repository, but cannot read the bearer
  or the mirror's node/age private material.
- `forkmesh-mirror` reads the bare repository and owns all encryption and node
  signing material. It is not an SSH login account.

The broker in
[`tools/ssh_authorization_broker.py`](../../../tools/ssh_authorization_broker.py)
creates a group-readable Unix socket in a directory that neither SSH-facing
account can write. It checks Linux `SO_PEERCRED` on every connection, permits
one role-specific bounded request, adds a fresh request ID, disables redirects
and ambient HTTP proxies, and validates the exact Worker response. The Git
gateway checks the broker's kernel-reported uid in the other direction. Thus a
Git process or repository hook cannot read or inherit the infrastructure
bearer.

Create `/etc/forkmesh/ssh-gateway.json` from the tracked
[`ssh-gateway.json.example`](../../../server/packaging/ssh/ssh-gateway.json.example) and
validate it with
[`ssh-gateway-config.schema.json`](../ssh-gateway-config.schema.json):

```json
{
  "schemaVersion": 1,
  "authorizationSocket": "/run/forkmesh-ssh-auth/authorize.sock",
  "authorizationBrokerUser": "forkmesh-ssh-auth",
  "gatewayExecutable": "/opt/forkmesh-mirror/ssh-gateway-entrypoint",
  "refreshNotifier": "/opt/forkmesh-mirror/ssh-refresh-notify",
  "repositoryRoot": "/srv/forkmesh-git",
  "limits": {
    "capacityDirectory": "/run/forkmesh-ssh-gateway",
    "maxConcurrentSessions": 16,
    "reservedReceiveSessions": 4,
    "maxConcurrentProcesses": 12,
    "reservedReceiveProcesses": 3,
    "uploadPackDeadlineSeconds": 900,
    "receivePackDeadlineSeconds": 600,
    "terminationGraceSeconds": 3,
    "receiveMaxInputBytes": 268435456,
    "defaultRepositoryMaxBytes": 17179869184,
    "storageScanDeadlineSeconds": 10,
    "cleanupDeadlineSeconds": 5,
    "maxPackThreads": 2
  },
  "repositories": [
    {
      "owner": "forkmesh",
      "name": "forkmesh",
      "path": "forkmesh-forkmesh.git",
      "access": "read-write",
      "maxStorageBytes": 17179869184
    }
  ]
}
```

Create the broker config from
[`ssh-authorization-broker.json.example`](../../../server/packaging/ssh/ssh-authorization-broker.json.example).
Only its dedicated account may read `/etc/forkmesh/ssh-gateway.token` (`0600`).
The token is never present in JSON, an environment variable, a Git process, or
an SSH wrapper. Configs, programs, wrappers, and every executable parent
directory are root-owned and not writable by the Git account.

Use a root-owned `0750` repository root and a dedicated repository-sharing
group. The bare repository itself can be owned by `forkmesh-mirror` with that
group and mode `2770`; add only `git` and `forkmesh-mirror` to the
group. Do not recursively make a private repository world-readable.
Install
[`forkmesh-ssh-gateway.conf`](../../../server/packaging/tmpfiles.d/forkmesh-ssh-gateway.conf)
through `systemd-tmpfiles` before enabling SSH. The capacity directory is
private to `git`; file locks in it bound concurrent forced-command sessions and
Git service processes across sshd children. Read sessions can use only the
general slots, while an authenticated, Worker-authorized push can also use the
reserved receive slots. Repository aliases that point at one bare repository
must use the same `maxStorageBytes` value.

Install the tracked broker service, fixed wrappers, and
[`99-forkmesh-git.conf`](../../../server/packaging/ssh/99-forkmesh-git.conf). The Match
block affects only `git`, retains the host's existing administrative
SSH policy, denies passwords, forwarding, TTYs, user rc files, and trusted-user
CA authentication, uses the separate lookup account, and accepts only Git's
`GIT_PROTOCOL` negotiation hint. A dedicated SSH-only daemon should
additionally use global `PermitRootLogin no`, `PermitUserEnvironment no`, no
broad `AcceptEnv`, and an `AllowUsers` list.
Those global settings must not be copied into a shared administrative daemon
without an explicit access migration.

The emitted authorized-key line also carries OpenSSH's `restrict` option and a
forced `serve --key-id …` command. The helper parses `SSH_ORIGINAL_COMMAND`
without a shell, accepts one `<owner>/<repository>.git` path, asks the Worker
for authorization, resolves the Worker's canonical namespace through the local
allowlist, and runs Git with a fixed argument array and a clean environment.
The root-owned entrypoint rebuilds the pre-Python environment from scratch,
carrying forward only OpenSSH's required `SSH_ORIGINAL_COMMAND` and the optional
`GIT_PROTOCOL` hint; the Python gateway then accepts only Git protocol versions
1 or 2 and an exact upload-pack/receive-pack command.
Command-line configuration disables repository hooks, pack-object hooks,
alternate-ref commands, fsmonitor programs, credential helpers, and the `ext`
transport. Upload and receive processes run in isolated process groups with
separate deadlines; timeout or gateway interruption terminates the complete
group, first with `SIGTERM` and then `SIGKILL` after the short grace period.
Both operations consume a global process slot and use a bounded pack-thread
count. A receive additionally takes a per-repository writer slot, asks Git to
enforce `receive.maxInputSize`, disables loose-object unpacking, checks the
repository's logical/allocated size before and after the transaction, and
leaves conservative space for pack indexes and transaction metadata. A failed
or interrupted receive reaps only transaction lock, temporary-pack, and
quarantine artifacts that did not exist when that receive began.

The configured storage value is an application limit, not a replacement for a
filesystem project quota or a separately mounted size-limited volume. Apply one
of those OS-level controls to each writable repository when a hard physical
disk boundary is required. The gateway fails closed if its capacity directory
or bounded storage scan is unavailable, if a repository is already at its
application quota, or if no eligible slot remains.

After a successful in-quota receive only, the gateway invokes the fixed
root-owned notifier with no arguments or inherited push data. A failed
notification never rewrites an already-committed push as failed.

The notifier atomically coalesces pushes into one marker. The tracked systemd
path/service then runs refresh as `forkmesh-mirror`, restarts only
`forkmesh-mirror.service`, and verifies identity-bound signed health over both
loopback and the gateway configuration's public Cloudflare origin using the
pinned Ed25519 node key before registering the active generation. Both checks
share one bounded deadline, ignore ambient proxies, and reject redirects. A
failed publication is retained as a fixed retry marker with a generic,
root-written state record and capped exponential backoff; it is never deleted
as if it succeeded. `ssh_post_receive_refresh.py ... status` exposes only the
pending/processing/retry phase, attempt count, and next retry time. The
`forkmesh-mirror-reconcile.timer` retries due work and performs a full
repository reconciliation every 30 minutes, while the independent renewal
timer republishes signed liveness between reconciliations. It never derives a
command, path, unit, or argument from a pushed ref, push option, repository
content, or SSH environment. See
[`ssh-post-receive-refresh.schema.json`](../ssh-post-receive-refresh.schema.json).

The bridge rebuilds every child environment from fixed literals and propagates
the packaged disk-backed
`TMPDIR=/var/lib/forkmesh-mirror/runtime-tmp` to refresh, restart, and
registration. It never trusts an ambient or repository-derived `TMPDIR`; this
keeps large encrypted-generation materialization out of the service's
`PrivateTmp` filesystem. As a bounded fallback for another local transient, the
bridge waits two seconds and retries only the idempotent `refresh` once within
the same 30-minute refresh deadline. It never retries `receive-pack`, service
restart, signed-health verification, or registration.

Validate before reloading sshd:

```bash
sudo systemd-tmpfiles --create \
  /usr/lib/tmpfiles.d/forkmesh-ssh-gateway.conf
sudo systemctl start forkmesh-ssh-authorization-broker.service
sudo -u git /opt/forkmesh-mirror/ssh-gateway-entrypoint check
sudo -u git \
  /opt/forkmesh-mirror/ssh-gateway-entrypoint worker-allowlist
sudo sshd -t
sudo sshd -T -C user=git,host=localhost,addr=127.0.0.1
sudo systemctl reload sshd
```

Keep an existing administrative session open, confirm its effective
configuration is unchanged, and open a second administrative connection before
closing the first. Never install the Match block until `sshd -t` succeeds.

Copy the single `worker-allowlist` output line into
`SSH_GATEWAY_REPOSITORIES`, then run `./deploy.sh secrets`. Regenerate and
repush it whenever the local gateway allowlist changes. This deliberate
duplication prevents the Web UI from advertising a repository that only exists
in the catalog and is not configured on the SSH host. The Worker also checks
the entry and its read-only/read-write mode again during authorization; the
gateway independently enforces its own local copy.

To disable SSH publication without relying on secret deletion, set
`SSH_GATEWAY_REPOSITORIES=disabled`, run `./deploy.sh secrets`, and verify the
catalog no longer returns `sshUrl`. The invalid allowlist fails closed. You may
then delete the four SSH secrets in Cloudflare. Merely removing an optional line
from `.env.production` does not erase an already-pushed Cloudflare secret.

For private repositories, operate the SSH origin on an owner-authorized machine
that is permitted to materialize that repository. Do not point an untrusted
mirror operator at private plaintext; ordinary private mirror replicas retain
their owner-sealed encrypted transport.

## Controller-free operation

The default fleet fails closed to the Worker: every `lookup` and `authorize`
is an HTTPS call, so a Worker outage stops all SSH access. A mirror meant to run
as an independent public Git server can authorize locally instead, through two
additive, opt-in mechanisms. Neither changes the default: a broker with no
`localAuthorization` block behaves exactly as before.

**Broker local authorization.** Add a `localAuthorization` block to the broker
config:

```json
"localAuthorization": {
  "mode": "fallback",
  "grantsFile": "/etc/forkmesh/ssh-local-grants.json",
  "dynamicRepositoriesFile": "/var/lib/forkmesh-mirror/gateway/dynamic-repositories.json"
}
```

- `"mode": "fallback"` keeps the Worker authoritative and consults the grants
  file **only when the Worker is unreachable** — never after an authoritative
  denial, so a cut uplink can never bypass a revocation. It requires `apiOrigin`
  and `gatewayTokenFile` to remain present.
- `"mode": "local"` never contacts a Worker; omit `apiOrigin` and
  `gatewayTokenFile` entirely. The grants file is the whole policy.

The grants file (`type: forkmesh.ssh-local-grants`) lists each authorized key,
its SSH public key, its account, and per-repository access:

```json
{
  "schemaVersion": 1,
  "type": "forkmesh.ssh-local-grants",
  "grants": [
    {
      "keyId": "sk_…",
      "publicKey": "ssh-ed25519 AAAA…",
      "account": "carol",
      "repositories": [
        {"owner": "forkmesh", "name": "forkmesh", "access": "pull-request"}
      ]
    }
  ]
}
```

`access` is `read-only`, `read-write`, or `pull-request`. A `pull-request` grant
may push only to `refs/heads/pr/<account>/*` (the gateway enforces this with
`receive.hideRefs`, so a contributor opens PR branches without write access to
`main`). An account's own registry forks are implicitly writable.

**Pull-request submission spool.** Set `pullSubmissionSpool` (and optionally
`pullSubmissionMaxBytes`) in the gateway config to accept signed submissions
through the forced command `forkmesh-pull-submit <owner>/<repo>` with the JSON
submission on stdin. The gateway verifies the submission's own Ed25519 signature
— the same `forkmesh-pull-event-v1` / `forkmesh-pull-comment-v1` canonical form
the Worker and desktop client use, whose author field is the raw public key, so
no key registry is required — and spools it durably with per-key and global
caps. A timer replays the spool to the controller once it is reachable:

```bash
python3 ssh_gateway.py --config /etc/forkmesh/ssh-gateway.json \
  spool-forward --api-origin https://app.forkmesh.com
```

Delivered submissions move to `forwarded/`, terminally rejected ones to
`rejected/`, and transient failures remain for the next run. Forwarding needs no
bearer token because the controller re-verifies the submission signature.

**Forking.** Set `dynamicRepositoriesFile` in the broker and
`dynamicRepositoriesPath` in the gateway config to the same file to enable
`forkmesh-fork <owner>/<repo>`. It clones a served repository into a new bare
repository under the requesting account (heads and tags only — the source
`forkmesh/pulls` ledger and internal `refs/forkmesh/*` are not copied), records
it in the registry, and refreshes the gateway. The forked repository becomes
locally serveable and pushable; publishing the fork to the global catalog still
flows through the owner's normal signed path.

The shipped
[`ssh-gateway.json.example`](../../../server/packaging/ssh/ssh-gateway.json.example)
and
[`ssh-authorization-broker.json.example`](../../../server/packaging/ssh/ssh-authorization-broker.json.example)
remain the read-only, Worker-authorized flagship setup; the fields above are the
optional additions for an independent write-capable mirror.

## DNS and Cloudflare transport

An ordinary orange-cloud Cloudflare DNS proxy does not transparently proxy raw
SSH on port 22. Use one of these technically valid arrangements:

- A DNS-only `A`/`AAAA` record to the hardened SSH origin.
- Cloudflare Spectrum (or an equivalent TCP proxy) in front of the SSH origin.
- Cloudflare Access/Tunnel with a required client-side `cloudflared`
  `ProxyCommand`; this is not a universally usable plain SSH URL and therefore
  should not be advertised as one unless every intended client is configured.

Keep the HTTPS Worker and SSH origin on separate hostnames. Apply network rate
limits, host patching, process/resource limits, and repository backups at the
SSH origin. A Cloudflare DNS record alone is discovery, not an SSH security
boundary.

## Failure and privacy behavior

- Unknown, revoked, malformed, and wrong-account keys all fail authentication
  without revealing the owning account.
- Missing, locally absent, read-only, and unauthorized repositories produce the
  same generic forced-command denial.
- The gateway never invokes an interactive shell, accepts arbitrary commands,
  or constructs a shell command from a repository path.
- Request paths, repository bytes, client IPs, SSH key material, and the bearer
  token are not written by the helper.
- The broker authenticates before schema or database work, returns no account
  principal, and binds each success to a fresh request ID plus the exact key,
  operation, and canonical repository response.
- Account rename retargets the key's blind-index account link; gateway lookup
  resolves the current account name rather than trusting registration-time
  encrypted metadata.
- Account deletion removes its SSH public-key rows.

## Round-robin gateways

`ssh.forkmesh.com` used to be one machine (mirror2). When that node was
retired the published `ssh://` clone URL died with it, because nothing else
in the fleet ran the gateway. The name now resolves to several mirrors and
each of them serves the flagship repository read-only:

```bash
tools/provision_ssh_gateway.sh <host-ip> [owner/name]
```

The script is idempotent and installs the whole node side (accounts,
programs, configs, broker unit, sshd `Match User git` block). It publishes
the bare mirror through a **read-only bind mount** under root-owned
`/srv/forkmesh-git`, because the gateway requires a root-owned repository
root while the node account must keep ownership of the repository it writes.
The sshd change is applied only after `sshd -t` accepts the merged config.

Pushes do not travel this path: mirrors are read-only replicas, and the
owner node pushes through its own authenticated fleet remotes. That is why
the Worker allowlist carries `forkmesh/forkmesh=read-only` and no
`mirror<N>/...` entries.

**A retired node rotates itself out of the push fan-out.** Those fleet
remotes (`forkmesh-mirror<N>-sync`) are ordinary Git remotes on the owner's
working copy, so decommissioning a node does not remove one. The desktop
fan-out runs every five seconds, and a host that no longer answers costs a
full TCP connect timeout on each pass. A gateway whose push fails at the
connection level — a connect timeout, a refused connection, a hostname that
no longer resolves — is therefore skipped for a cooldown that doubles from
one minute up to fifteen, while every gateway that is up keeps syncing on
the normal pass; the first success puts it straight back into rotation.
A release rollout still tries every configured gateway, so an operator
pushing a tag gets an honest per-gateway count. Failures that reach the
repository (a refused key, a hook denial) are not rotated out: they answered,
and they stay loud on every pass.

The network log says it once, then hourly, and after a full day of silence
names the remote to drop:

```bash
git -C <working-copy> remote remove forkmesh-mirror13-sync
```

**Host certificates give the members one identity.** Every member keeps its
own host key — no private key is ever shared between machines — and
additionally presents a certificate signed by the ForkMesh host CA that
names `ssh.forkmesh.com` as a principal. Clients trust the CA once instead
of pinning four keys:

```text
@cert-authority ssh.forkmesh.com,*.forkmesh.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII5OvM7x8w4o1auHqYiSObIWHaVdAy4tT8dxLOai76Vs
```

Sign (and later rotate) with:

```bash
tools/sign_ssh_host_certs.sh <host-ip> [<host-ip> ...]
```

The CA private key lives only on the operator machine
(`host_ca_ed25519` in the project's ssh folder, `~/projects/forkmesh/ssh/`
on the projects layout, mode 0600; override with `FORKMESH_SSH_HOST_CA`) and is
never uploaded; certificates are valid one year, so re-run the script well
before expiry. `HostCertificate` is a global sshd directive and therefore
gets its own include file, applied only after `sshd -t` accepts it.

The App Worker carries the gateway secrets because every authorization API is
served from `app.forkmesh.com/api/*`. Run `./deploy.sh secrets` from `app/` to
publish them without changing the mirror-router trust key.
