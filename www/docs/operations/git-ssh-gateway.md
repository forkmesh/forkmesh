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

Two optional settings harden the token:

```dotenv
SSH_GATEWAY_TOKEN_NEXT=<the next token, during a rotation>
SSH_GATEWAY_ALLOWED_ADDRESSES=203.0.113.7,2001:db8:7::/64
```

`/api/ssh/authorize` compares the bearer token in constant time against
`SSH_GATEWAY_TOKEN` and, when set, `SSH_GATEWAY_TOKEN_NEXT`; either is
accepted, so a token rotates without downtime: set the new one as `_NEXT`,
move the gateway host to it, then make it `SSH_GATEWAY_TOKEN` and remove
`_NEXT` (`install_ssh_relay.sh --rotate-token`, then `--finish-rotation`).
`SSH_GATEWAY_ALLOWED_ADDRESSES` additionally pins the gateway host: authorize
calls from any other address (as the edge sees it, `cf-connecting-ip`) get
401 even with the right token. A malformed list admits nobody.

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
  the host, so a CDN's request-size limit does not apply. Without it the
  origin's name is resolved through DNS, as on a separate front host (see
  [Keep the origin private](#keep-the-origin-private)).
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

The relay's HTTP requests carry `User-Agent: git/forkmesh-ssh-relay`; the
broker's authorize calls carry `ForkMesh-ssh-authorization-broker/1.0`.

For users, once a key is registered in Settings:

```sh
ssh -T git@ssh.example.org                   # a greeting; there is no shell
git clone git@ssh.example.org:owner/repository.git
git push origin HEAD:pr/<account>/my-fix     # contributor branch, where allowed
```

## Keep the origin private

An instance behind a CDN proxy hides its address: its HTTPS name resolves to
the CDN, and its firewall admits only the CDN's ranges. SSH cannot go
through an HTTP proxy, so the SSH name needs a DNS-only record, and that
record is the address of whatever host runs the gateway. With the gateway
on the instance itself, `dig ssh.example.org` publishes the origin.

Run the gateway on a separate, disposable **front host** instead. In relay
mode it needs no repository storage and no instance secrets: it reaches the
control plane and the HTTPS Git routes only through the public HTTPS name,
resolved through ordinary DNS (to the CDN) with ordinary TLS verification.
Its configuration has no `connectAddress` and no `apiConnectAddress`, so the
origin's address appears nowhere on it. It holds two secrets: the gateway
token and its own SSH host key.

1. **A small VPS** (1 vCPU, 512 MB-1 GB, any provider; Debian or Ubuntu
   with OpenSSH 8.5 or newer and Python 3), and a source checkout on it,
   e.g. `git clone --depth 1 <source> /opt/forkmesh-src`. Firewall: the SSH
   port in, HTTPS out.
2. **On the instance**, issue the token and publish the SSH addresses
   without installing a gateway there:

   ```sh
   sudo bash tools/install_ssh_relay.sh --control-plane-only --host ssh.example.org
   ```

3. **Copy the token to the front over SSH, straight into the installer**, so
   it is never written anywhere else (not to a shell history, a clipboard or
   a temporary file):

   ```sh
   sudo cat /etc/forkmesh-ssh/ssh-gateway.token |
     ssh root@front 'bash /opt/forkmesh-src/tools/install_ssh_relay.sh \
       --remote-front --api-origin https://git.example.org \
       --host ssh.example.org --token-file -'
   ```

   `--print-config` instead of installing shows the configuration it would
   write. `--check` on the front probes `/api/ssh/authorize` through the
   CDN and says whether the token was accepted.
4. **DNS:** a DNS-only (not proxied) `A`/`AAAA` record for `ssh.example.org`
   pointing at the front only. The HTTPS name stays proxied.
5. Optionally pin the front on the instance:
   `SSH_GATEWAY_ALLOWED_ADDRESSES=<front address>` in `app.env`, then restart
   the app.

**CDN limits.** Everything the front relays crosses the CDN like any HTTPS
client's traffic:

- *Request size.* Cloudflare's Free and Pro plans accept request bodies up to
  100 MB (Business 200 MB, Enterprise 500 MB by default). A push is one
  request, so the front sets `maxRequestBytes` (`--max-request-mb`, default
  100; 0 for none) and stops a larger push before uploading it, telling the
  user to push fewer commits at a time. A 413 from the proxy itself gets the
  same explanation; the repository quota's own 413 keeps its message. Fetches
  are unaffected (responses are not capped).
- *Time.* Cloudflare waits up to 100 seconds for the origin's response to
  start. A push whose server-side checks take longer gets a 524; the user is
  told the push may still have landed and to `git fetch` before retrying.
  Large pushes are streamed chunked; the front's own socket timeout is 10
  minutes.
- *Bot protection.* Git clients are not browsers and cannot solve a
  challenge. If the zone challenges the front (a response with
  `cf-mitigated`), the user gets a message naming bot protection. Add a WAF
  custom rule with action *Skip* (managed rules, rate limiting and, on paid
  plans, Super Bot Fight Mode) matching
  `ip.src eq <front address> and lower(http.user_agent) contains "forkmesh-ssh"`,
  or turn off Bot Fight Mode, which a Free plan cannot skip. Match the
  front's address, never the bearer token: a token in a rule is a secret
  copied into the CDN's configuration.

**Abuse controls on the front.** The front sees real client addresses (the
origin sees only the CDN's). `--remote-front` adds:

- `59-forkmesh-front-limits.conf`: `MaxStartups 10:30:60`,
  `PerSourceMaxStartups 4` and `LoginGraceTime 20` for unauthenticated
  connections (global settings, fine on a dedicated host);
- `maxSessionsPerClient: 4` in `httpGit`: at most four concurrent Git
  sessions from one address (IPv6 by /64), on top of the host-wide
  `limits`;
- a fail2ban jail, `forkmesh-git-ssh` (the `sshd` filter in aggressive mode,
  10 failures in 10 minutes bans for an hour). It is written either way and
  runs once `fail2ban` is installed.

**Trade-offs.** Every SSH operation takes an extra hop through the front and
the CDN, and inherits the CDN's size and time limits. The front is a single
point of failure for SSH (HTTPS is unaffected): run two fronts with the
same token behind two DNS records, or keep a spare that a DNS change can
promote. The front is disposable: rebuild it from a checkout and the token,
and nothing else is lost (give it the same host key, or users see a host key
warning).

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
principal. The per-client session cap keys its lock files by a truncated hash
of the client address, in `/run` only. Account renames follow the key; account deletion removes its keys.
