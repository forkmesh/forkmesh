# Secrets vault

The app's variables (`ADMIN_PASS`, the router seed, integration tokens and so
on) are stored in the database encrypted under `DATA_KEY`. `DATA_KEY` is kept
only on the operator box; on a node it exists only in the app's memory while
that node is unsealed.

Switches in `deploy/fleet.toml` `[app]`: `vault = true` turns the vault on
(the example config defaults to plain systemd credentials);
`vault_peer_unseal = true` lets a sealed node get the key from an unsealed
peer, so the steady state needs no operator.

## What is where

| Item | Location |
|---|---|
| `DATA_KEY` | operator box only: `config/fleet/DATA_KEY` (0400, preferred) or the `DATA_KEY=` line of `secrets.env`; on a node only in unsealed memory |
| `RQLITE_PASSWORD`, `FORKMESH_MESH_SECRET` | bootstrap credentials: host-key encrypted systemd credentials on each node |
| `FORKMESH_CERTSYNC_SECRET` | credential of the certsync units only, never the app |
| every other app variable (`[app] secrets` and `optional_secrets`) | `host_secrets` rows, sealed |
| DNS provider token | not in the vault; stays on the operator box |

## Format

Implemented in `app/host/forkmesh_host/vault_crypto.py`, shared by the
operator tool and the nodes.

- Key: `HKDF-SHA256(DATA_KEY, salt "fmnext-vault-v1", info "fmnext-host-secrets")`, AES-256-GCM.
- Random 96-bit nonce per row, renewed on every sync.
- AAD `"fmnext-secret-v1\0" + name`, so rows cannot be swapped.
- `key_id`: first 16 hex of `SHA-256(HKDF(..., info "fmnext-key-id"))`; a wrong
  key is detected without a decrypt oracle.
- `__vault_check__` seals a fixed plaintext; every candidate key is proven
  against it with a linearizable read.
- Fail closed: unsealing fails if any row fails authentication.

Encryption happens only on the operator box (`tools/next_vault.py sync`);
nodes only decrypt.

## Node lifecycle

1. **Sealed start.** With `FORKMESH_VAULT=1` only bootstrap credentials load
   and `DATA_KEY` is dropped from every source. Requests answer
   `503 {"error":"sealed"}`, `/readyz` is not ready (so the node leaves DNS and
   deploy gates wait), cron and queues stop, and the membership row is
   `sealed`, so the node hosts no Durable Objects.
2. **Unseal** from a peer (at start and every 15 s while sealed) or from the
   operator (`next_vault.py unseal` pipes the key over SSH stdin into
   `fmnext-unseal`, which writes it to the 0600 unix socket
   `/run/forkmesh-next/unseal.sock`; only root and the app user may connect).
   The node checks `key_id` and `__vault_check__`, decrypts every row onto
   `env`, and rejoins.
3. **Reload** after `sync` without a restart.
4. **Seal**: `next_vault.py seal nN` drops every value and derived key cache;
   `--restart` (`seal-restart`) also overwrites the bytes and ends the process.
   The IDS lockdown uses it.
5. **Quarantine**: a quarantined node refuses unseal and peers refuse it; after
   a release peers wait until the operator has unsealed it once ([IDS.md](IDS.md)).

Five failed socket unseals within 60 s lock further attempts for the window;
failed peer keys do not count. Node operator actions (`fmnext-app-ops`) run
inside the unsealed app over the socket, so no second process holds the key.

## Peer unseal

A sealed node sends `POST /_vault/peer-unseal` to a peer's mesh listener. The
peer answers only if it is unsealed; the request has a valid mesh HMAC within
30 s and a single-use nonce; the requester is in the static
`FORKMESH_MESH_PEERS` map and connects from its own WireGuard address (bound
to its WireGuard private key; the firewall admits the port only on the mesh
interface); the requester has a fresh `sealed` membership row; rate limits
allow it; and the requester is not quarantined.

The request carries an ephemeral X25519 public key; the answer is `DATA_KEY`
encrypted to it (X25519, HKDF-SHA256, AES-256-GCM, transcript as AAD), so
observers and replays learn nothing. Every answer and refusal is logged
without the key. Only a restart of every core at once needs the operator.

## Threat model

Protects against: stolen disks, snapshots, database copies and backups (only
ciphertext); a node's credential files (no `DATA_KEY` or app variable on
disk); someone holding only bootstrap credentials; swapped, replayed or
tampered rows; the key reaching disk (the unit sets `MemorySwapMax=0`,
`LimitCORE=0`, `ProtectProc=invisible`, and the process is non-dumpable; the
key never appears in argv, environment or logs).

Does not protect against: root or code execution as the app on a live,
unsealed node (it can read memory, or impersonate that node to a peer, which
is the same exposure); a compromised operator box; Python's inability to
securely erase strings after sealing.

## Availability

A node that restarts alone is unsealed by peers within about 15 s. If every
core restarts while the operator box is off, the site stays sealed until one
node is unsealed. Keep `fmnext-unseal.timer` running; it sends no key while
nodes are unsealed.

## Operator commands

```sh
python3 tools/next_vault.py status
python3 tools/next_vault.py sync --execute
python3 tools/next_vault.py sync --prune --execute
python3 tools/next_vault.py unseal [nN] --execute
python3 tools/next_vault.py unseal --if-sealed --execute
python3 tools/next_vault.py seal nN [--restart] --execute
```

Keep `DATA_KEY` in its own 0400 file with an offline backup rather than in
`secrets.env`.

## Rotation

Offline rotation uses `tools/next_rekey.py` ([REKEY.md](REKEY.md)): re-key a
backup, load it with the apps stopped, then `sync` re-seals the vault. There
is no online rotation; `sync` refuses to write when any row was sealed under
another key. After a suspected key compromise, rotate individual secrets and
treat data encrypted under `DATA_KEY` as exposed until re-keyed.
