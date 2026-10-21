# Group-call server on the cores

`server/cmd/forkmesh-calls` is ForkMesh's own WebRTC selective forwarding unit
(SFU) for group audio and video calls ([server/rtc.md](../server/rtc.md)). The
fleet runs it on **every core** as `fmnext-calls.service`. `[calls]` in
`deploy/fleet.toml` controls it, `deploy/core/render.py` renders it, and the
`calls` step of `tools/next_fleet.py` installs it.

## Redundant on every core

A room has to live in one process, but no core is a fixed calls node. Every
core runs the server, and the app gets the full list of them in
`FORKMESH_CALLS_NODES`. That is a comma-separated list of
`wss://nN.next.forkmesh.com/calls/ws`, one per core in `[[nodes]]` order,
rendered into every node's `app.env`, so `add-core` and `remove-core` keep it
up to date. When the app issues a join ticket, it places each room by
rendezvous hashing over the nodes that answer
`https://nN.next.forkmesh.com/calls/health`. If a client's node dies, the
client joins again and lands on a surviving node.

- Losing one or two cores keeps calls working. Calls on the lost core
  reconnect to a surviving one.
- Every core renders the same files (`deploy/tests/test_no_main_node.py`,
  `deploy/tests/test_calls.py`). Only the node name and the node's own public
  addresses differ.
- App replicas (`aN`, autoscaled) run no call server and have no per-node name.
  They still get the node list in their `app.env`.

## What runs where

| Piece | Setting |
| --- | --- |
| Binary | `/usr/local/bin/forkmesh-calls`, a static Go build (`CGO_ENABLED=0`, linux/amd64) made on the operator box |
| Unit | `fmnext-calls.service`, user `fmnext-calls`, `Restart=on-failure`, `MemoryHigh=200M`, `MemoryMax=256M`, `GOMEMLIMIT=180MiB`, no swap, `ProtectSystem=strict`, no capabilities, `MemoryDenyWriteExecute` |
| Signalling | HTTP on `127.0.0.1:<http_port>` (default 7881), reached only through Caddy's `handle /calls/*` in the core's per-node block `nN.next.forkmesh.com`. The shared names `app.forkmesh.com` and `world.forkmesh.com` do not route `/calls/`. |
| Media | UDP `<udp_port>` (default 3479) on all addresses. It is open in nftables on every core, in the AWS security group (`tools/next_aws.py ingress_rules`) and in the IDS listener list. |
| Advertised addresses | `FORKMESH_CALLS_ADDRESSES`: the node's `public_ipv4` and, when set, `public_ipv6` (from `fleet-instances.json`) |
| Allowed browser origins | `FORKMESH_CALLS_ORIGINS`: `https://<served app host>` (`app.forkmesh.com`), `https://app.next.forkmesh.com`, plus any `[calls] origins` |
| Outbound | `IPAddressDeny=` blocks the private, CGNAT, link-local, documentation and reserved ranges, the same list as the TURN relay. Loopback stays open because Caddy proxies over it. |
| IDS | service user `fmnext-calls` (unit `fmnext-calls.service`, exe the binary), TCP listener 7881, UDP listener 3479, and the unit in the lockdown stop list with its `50-fmnext-ids.conf` drop-in |

Memory: a core has about 1 GB. These limits sit beside the app (340M max),
rqlite (320M max), the daemon (128M max) and coturn (200M max). They follow
the TURN relay's limits, with a little more room for forwarding.

## `[calls]`

```toml
[calls]
enabled = true
udp_port = 3479
http_port = 7881
# origins = ["https://forkmesh.com"]   # extra browser origins, https only
```

The renderer refuses the following:

- unknown keys, including the old `node`;
- a UDP port outside 1025-10239, which keeps it inside the range the IDS
  watches (`udp_max_port`);
- a port another core service already uses (TURN, WireGuard, rqlite, the app
  ports);
- `FORKMESH_CALLS_SECRET` missing from `[app] secrets`/`optional_secrets`;
- `FORKMESH_CALLS_NODES` set in `production.vars`.

`next_fleet.py fleet-toml --execute` keeps the live `[calls]` table, as it
keeps `[turn]`.

## The secret

`FORKMESH_CALLS_SECRET` is one value shared by the app and every core. The
app signs join tickets with it, and the SFU checks them
([server/rtc.md](../server/rtc.md)).

- `next_fleet.py init-secrets --execute` generates it (`token_urlsafe(48)`)
  into `secrets.env`.
- The app reads it from the vault. It is listed in `[app] optional_secrets`
  and sealed by `next_vault.py sync`.
- Each core also holds it as the encrypted credential
  `/etc/forkmesh-next/creds/FORKMESH_CALLS_SECRET.cred`, which the unit loads
  with `LoadCredentialEncrypted=`. No shell wrapper and no file in tmpfs: the
  binary reads `$CREDENTIALS_DIRECTORY/FORKMESH_CALLS_SECRET` itself. With the
  vault on, the `secrets` step keeps this file on the cores
  (`node_local_credential_names`) and removes it from app replicas.
- The `calls` step and `roll` refuse a value shorter than 32 characters, one
  that is not URL-safe, or one that reuses another secret. They refuse before
  draining any node.

## Steps

- **`calls`** (in `INSTALL_STEPS`, `ROLL_STEPS` and add-core's services step;
  cores only):
  1. Builds `server/cmd/forkmesh-calls` and stages it in
     `/tmp/fmnext-calls` (root, 0700).
  2. Creates the `fmnext-calls` user if needed.
  3. Installs the binary only if it changed.
  4. Re-encrypts the credential only if the decrypted value differs. The
     value goes over SSH stdin and never into an argument.
  5. Restarts the unit only if the binary changed, or if the secret the
     running process loaded (`/run/credentials/fmnext-calls.service/...`)
     differs.

  An ordinary roll for an unrelated `server/` change therefore does not drop
  any calls. The IDS expectation of the step rescans the users files, the
  binary, the credentials directory and the unit wants.
- **`files`** restarts `fmnext-calls.service` only when its unit file
  changed.
- **`start`** enables it (`enable --now`). The unit has
  `ConditionPathExists=` on the binary and the credential, so a core that has
  not run the `calls` step yet skips it, and the app's health probe leaves
  that node out.
- **`remove-core`** stops it with the other services.
- Phone-approved self-deploy ([self-deploy.md](self-deploy.md)) rolls only
  the mirror daemon. A change under `server/rtc/` reaches the cores through the
  desktop's auto-deploy roll or `next_fleet.py roll`.

## Go live

1. Run `init-secrets` and `next_vault.py sync` from this branch before the
   merge. The auto-deploy roll after the merge needs `FORKMESH_CALLS_SECRET`
   in `secrets.env`. Without it the roll stops before draining any node.

   ```sh
   python3 tools/next_fleet.py init-secrets --execute
   python3 tools/next_vault.py sync --execute
   ```

2. Merge to `main`. Auto-deploy builds the release and, because `server/` and
   `deploy/core/` changed, runs `next_fleet.py roll --execute`. With
   `manual-roll` set, run that yourself.
3. Check:

   ```sh
   for n in n2 n3 n4; do curl -fsS https://$n.next.forkmesh.com/calls/health; echo; done
   python3 tools/next_ids.py status n2 n3 n4
   ssh root@<core> systemctl status fmnext-calls
   ```

## Rotate the secret

Edit `FORKMESH_CALLS_SECRET` in `secrets.env`, then run
`next_vault.py sync --execute` (the app reloads it), then the `calls` step on
each core:

```sh
for n in n2 n3 n4; do python3 tools/next_fleet.py install $n --steps calls --execute; done
```

The `calls` step sees that the running process loaded a different secret and
restarts the server. Calls in progress drop and rejoin. Tickets the app
signed with the old secret stop working on cores that already restarted, but
tickets live only 60 s.

## Turn it off

Set `[calls] enabled = false` and roll (`files,start`). The unit file,
binary and credential stay on the nodes, so stop them by hand there with
`systemctl disable --now fmnext-calls`.
