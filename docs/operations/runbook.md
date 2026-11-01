# Fleet operations runbook

Part of [Operations](README.md).

How to bring up, deploy, check and tear down the fleet: three core nodes
(currently `n3`, `n4` and `n5`; `n1` and `n2` are retired, `retired_cores` in
`deploy/fleet.toml`) and a runner (`r1`) on Vultr. Cloudflare serves DNS only
for the app and world names. Take node names from `[[nodes]]` in
`deploy/fleet.toml` when the fleet changes. `deploy/core/render.py` renders
node files from `deploy/fleet.toml`. Design: [served stack](served-stack.md);
tool map: [fleet tooling](fleet-tooling.md); operator folders and files:
[operator files](operator-files.md).

Every tool dry-runs by default; nothing changes without `--execute`. Run
from `~/projects/forkmesh/src`. No tool prints a secret or passes one on a
command line. The tools need Go, Python 3.13, and Node with wrangler for the
www Worker.

## Bring up

```sh
python3 tools/next_fleet.py plans; python3 tools/next_fleet.py list
for n in 1 2 3; do python3 tools/next_fleet.py create-core $n --execute; done
python3 tools/next_fleet.py create-runner 1 --execute
python3 tools/next_fleet.py fleet-toml --execute
python3 tools/next_fleet.py init-secrets --execute
for n in n3 n4 n5; do python3 tools/next_fleet.py install $n --steps base,wireguard --execute; done
python3 tools/next_fleet.py fleet-toml --execute      # adds WireGuard public keys; commit it
for n in n3 n4 n5; do python3 tools/next_fleet.py install $n --steps rqlite,turn,calls,files,secrets,start --execute; done
python3 tools/next_dns.py ensure --execute
python3 tools/next_deploy.py build --execute
python3 tools/next_deploy.py upload --execute
python3 tools/next_deploy.py migrate --execute
python3 tools/next_vault.py sync --execute
python3 tools/next_deploy.py restart --execute
for n in n3 n4 n5; do python3 tools/next_fleet.py install $n --steps daemon --execute; done
python3 tools/next_fleet.py install r1 --execute
python3 tools/next_fleet.py install r1 --steps daemon --execute
```

`create-*` adopts an existing instance by label, so re-running is safe.

Operator-box user units (copy from `deploy/operator/`, then
`systemctl --user enable --now`): `fmnext-backup.timer`, `fmnext-unseal.timer`,
`fmnext-dns-health.service`. Enable lingering for the user.

The homepage Worker deploys by hand from `www/wrangler.production.toml`
([website](../www/README.md), [auto-deploy](auto-deploy.md)).

## Verify

```sh
ssh -i $K root@<core> "curl -fsS -K /etc/forkmesh-next/rqlite.curlrc 'http://127.0.0.1:4001/nodes?nonvoters'"
curl -fsS --resolve app.forkmesh.com:443:<ip> https://app.forkmesh.com/readyz
curl -fsS https://n3.next.forkmesh.com/readyz
python3 tools/next_dns.py status
python3 tools/next_ids.py status n3 n4 n5 r1
```

## Routine operations

- **Deploy.** `next_deploy.py all --execute` (autodeploy runs it on every
  merge, [auto-deploy](auto-deploy.md)): migrate through any
  healthy node, then restart one node at a time in a rotating order. Each node is
  gated on loopback `/readyz`, `/api/version` reporting the new rev, and Caddy
  serving `/readyz`; a failing node goes back to its previous release and the
  rollout stops. Activation scripts hold a lock, survive a dropped SSH
  session, and are verified by comparing `current/BUILD_REV` with the running
  version. Roll back one node with `next_deploy.py rollback nN --execute`.
- **Node files.** After editing `deploy/core/templates` or `deploy/fleet.toml`,
  run `install nN --steps files,start --execute` one node at a time.
- **Rotate a secret.** Edit `secrets.env`, then `next_vault.py sync --execute`;
  unsealed nodes reload without a restart. Bootstrap credentials
  (`RQLITE_PASSWORD`, `FORKMESH_MESH_SECRET`) need `install nN --steps secrets`
  and a unit restart. `DATA_KEY` is rotated only offline ([re-keying](rekey.md)).
- **Vault.** `next_vault.py status` shows sealed/unsealed per node and key ids.
  A restarted node is unsealed by its peers within about 15 s; the operator
  timer acts only when every node is sealed ([vault](vault.md)).
- **Backups.** `fmnext-backup.timer` runs `next_backup.py run --execute`
  nightly: it pulls `/db/backup?compress` over an SSH tunnel, keeps it only
  after `PRAGMA integrity_check` and table counts pass, encrypts it to the
  backup public key, and retains 7 daily and 4 weekly copies. Only files with
  the exact backup name pattern are ever deleted
  ([disaster recovery runbook](../disaster-recovery/runbook.md)).
- **Certificates** are leaderless (`deploy/core/certsync.py`). The
  rendezvous-first live core renews when the freshest pair anywhere is
  missing or within 30 days of expiry; every core pulls the freshest valid
  pair every 5 minutes. `journalctl -u fmnext-cert-sync` shows the mode
  (`files`, `acme`, `internal`).
- **Cron** is leaderless: the per-minute coordinator rotates across live
  nodes and hands over 45 s into each minute; `host_cron_runs` records who ran
  which minute. Keep node clocks in sync.
- **TURN relay and group calls.** Every core runs the office TURN relay
  (`fmnext-turn`, [Office](../office/README.md)) and the group-call
  server (`fmnext-calls`, [call servers](call-servers.md)). Their shared
  secrets (`OFFICE_TURN_SECRET`, `FORKMESH_CALLS_SECRET`) come from
  `init-secrets`, reach the app through `next_vault.py sync`, and reach each
  core as an encrypted credential through the `turn` and `calls` steps of a
  roll. Check calls with
  `curl -fsS https://nN.next.forkmesh.com/calls/health`.
- **Node DNS reconciler** (`app/host/forkmesh_host/dns_reconciler.py`): every
  unsealed node probes every core and votes in `dns_health_votes`. It acts
  only with a DNS token in the vault and a majority of voters; without one
  the fleet runs `[dns] reconciler = "dry"` and only votes.

### DNS health failover

`tools/next_dns.py health-loop --execute` runs as the `fmnext-dns-health`
user unit on the operator box and manages `app.forkmesh.com`,
`world.forkmesh.com` and their `next` aliases (one A record per member, AAAA
for members with IPv6).

- Every 30 s it sends a TLS-verified `GET /readyz` to each core. A sealed,
  quarantined or locked-down node fails. A read-only core (no rqlite leader,
  503 with `"readOnly": true`) counts as healthy only while no core is fully
  ready, so a lone survivor stays in DNS and serves reads
  ([read-only mode](served-stack.md#read-only-mode)).
- A node leaves every name after 3 failed probes and returns after 2 minutes
  healthy. At most one change per 5 minutes. A set is never emptied.
- A round in which every core fails is treated as an operator-box outage and
  changes nothing.
- `next_dns.py drain nN --execute` removes a node and holds it out for
  maintenance; `undrain` releases it. Wait out the 60 s TTL after a drain.
  Never pipe `drain` into `head`.
- Rules live in `HealthState` (`tools/next_dns.py`) and
  `app/host/forkmesh_host/dns_rules.py`; state in `config/fleet/dns-health.json`.
  Restart the unit after changing `next_dns.py`.

### Adding, removing and replacing cores

- **Add:** `next_fleet.py add-core --execute` or the desktop's Add core node;
  the new core joins as a non-voter while three voters exist and enters DNS
  only when healthy.
- **Remove:** `next_fleet.py remove-core nN --execute`. Refused below three
  cores or three healthy voters; a non-voter is promoted first. The removed
  core is listed in `[hosted] retired_cores` so repositories its node account
  owns still take pushes ([core push](core-push.md)).
- **Replace:** `next_fleet.py add-core --replace nOLD --execute` or the
  desktop's **Replace…** on a core row. One run adds the new core, removes the
  old one (its instance is kept) and commits both on `fleet/add-core-nNEW`
  ([fleet tooling](fleet-tooling.md#replacing-a-core-node)).
- **IPv6:** drain, `next_fleet.py enable-ipv6 nN --reboot --execute`,
  `fleet-toml --execute`, `next_dns.py ensure --execute`, undrain.

Full order of operations: [fleet tooling](fleet-tooling.md#adding-a-core-node).

### Signup challenge

The app verifies a Cloudflare Turnstile token itself (`app/src/turnstile.py`)
on `POST /api/accounts/signup` and `POST /api/accounts/forgot-password`.
Signed node registrations are not gated but have a per-IP limit.

- Site key: `TURNSTILE_SITE_KEY` in `app/wrangler.toml` `[vars]` (the cores
  read that file's `[vars]` as app settings). Secret:
  `TURNSTILE_SECRET_KEY` in `secrets.env` and the vault (`optional_secrets`).
- Each `GET /signup` issues a claim cookie; a token from an earlier page view
  is refused with `turnstile_stale_page`.
- Fails closed: without the secret, or on a siteverify error or timeout, the
  forms answer 503 `turnstile_unavailable`. Signup then takes the backup
  check instead ([signup backup check](../app/signup-challenge.md#signup-backup-check)),
  and so does password reset
  ([password reset challenge](../app/signup-challenge.md#password-reset-challenge)); set
  `SIGNUP_BACKUP_CHECK=off` to make both fail closed too.
- Rotate in the Turnstile dashboard, update `secrets.env`, then
  `next_vault.py sync --execute` within the grace period.
- Off the VPS platform with no secret, Turnstile's always-pass test keys are
  used locally. On a VPS they are accepted only with `TURNSTILE_TEST_MODE=1`,
  which must never be set in the fleet.

### Native app builds

Codemagic builds the native apps from a private copy of this repository on
the cores, never from a public forge.

- Each core keeps a bare repository under `/var/lib/forkmesh-next/git/`.
  Caddy serves it read-only over Git's dumb-HTTP protocol under `/git/`
  behind basic auth (`GIT_CI_PASSWORD` in `secrets.env`; only the bcrypt hash
  is on the node).
- `tools/next_git_host.py` (`setup`, `push`, `url`) manages it;
  `tools/next_codemagic.py build <workflow> --execute` pushes and starts a
  build, `status <id> --logs` shows step logs with the password masked.
- Dumb HTTP cannot serve shallow clones, so every workflow sets
  `CM_CLONE_UNSHALLOW: "true"`.
- Rotate the password with `next_git_host.py setup --execute` after removing
  its line from `secrets.env`, then give Codemagic the new repository URL.
- Signed workflows need the App Store Connect variable group on the
  Codemagic app.

### Caddy version

The cores run a pinned upstream Caddy from `/usr/local/bin/caddy`, not the
distribution package. `[caddy]` in `deploy/fleet.toml` pins the version and
tarball sha256; verify the tarball against the release checksums and their
signature before placing it in `data/fleet/dl/`.

- Install or upgrade: `next_fleet.py install nN --steps caddy --execute`. It
  verifies the tarball and binary, validates the live Caddyfile with the new
  binary, installs a drop-in that only changes `ExecStart`/`ExecReload`,
  restarts, and checks `/readyz`. On failure it runs `caddy-debian`, which
  restores the packaged binary.
- The IDS expects both files through a signed expectation.
- Roll out one node at a time, drained from DNS, and reload twice checking
  `journalctl -u caddy` for panics.
- The Caddy unit has `Restart=on-failure`; deploy gates restart Caddy once if
  it stops serving.

## Recovery

- **Every node restarted:** the site answers 503 `{"error":"sealed"}` until
  one node is unsealed (`next_vault.py unseal --execute` or the timer); peers
  unseal each other after that.
- **A node will not unseal:** check `journalctl -u forkmesh-app | grep vault:`.
  `wrong_key`/`vault_tampered` mean a key mismatch, `vault_empty` means `sync`
  never ran, `db_unavailable` means no rqlite leader yet.
- **Lost `DATA_KEY`:** the vault and every encrypted row are unrecoverable.

## Destroy

`next_fleet.py destroy r1 --yes --execute` (a runner holds no state).
Destroying a core is refused while fewer than three healthy voters would
remain. Tearing down the cluster: back up first, set `[dns] reconciler =
"off"`, then `destroy nN --yes --execute --force`. `--force` skips only the
quorum check; the label and tag checks cannot be bypassed.

## Cost

About $5 per month per 1 GB instance, DNS and the www Worker on a free plan,
no object storage. Burst runners are billed hourly on top.

## Known gaps

- Country lookup: Caddy sets no country header on direct requests.
- The restore drill into a throwaway instance is not automated.
- Runner CLIs are installed and signed in through the relayed login
  ([runners](runners.md)).
- Monitoring and intrusion detection: [intrusion detection](intrusion-detection.md), [node logs](node-logs.md).

## Security hardening

Enforced by `deploy/core/render.py` and `tools/next_fleet.py`:

- **Caddy:** admin API on a unix socket only; header, body and idle
  timeouts; body caps (16 MB app/world, larger for pull uploads and Git
  pushes); HSTS and `nosniff` on every response; sandboxed unit.
- **App host:** requests for hosts outside `FORKMESH_PUBLIC_HOSTS` get 421;
  the edge header is verified before the app sees the request; outbound
  calls go through an egress guard (`FORKMESH_EGRESS_ALLOW` is the only
  exception list).
- **certsync:** pairs must chain to a system CA, cover the name and expire
  within 100 days; the pair server uses its own `FORKMESH_CERTSYNC_SECRET`,
  which the app does not hold.
- **nftables output rules by socket uid:** rqlite ports only from rqlite,
  root and the app; mesh and certsync ports only from their services;
  metadata service only from root.
- **System:** no swap for secret-holding services, `ptrace_scope = 1`,
  hardened sshd (no forwarding except local, no password auth), root
  password locked, vendor console users removed.
- **Runner:** jobs run as an unprivileged job user with no access to the node
  key or daemon config; nftables refuses the job user private, metadata,
  loopback and mesh addresses.
- **Operator box:** encrypt the disk, protect the fleet key with a
  passphrase, use a scoped cloud sub-user key, keep `DATA_KEY` and the backup
  decryption key offline, enable account 2FA.
- CAA records apply to the whole zone; add them only after checking every
  issuer in use.

Rollout order for node-file hardening: node files on every core first (they
render the host allowlist), then the app release, then the runner.
