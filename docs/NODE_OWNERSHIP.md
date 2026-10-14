# Fleet node ownership and version fields

Status: 2026-09-24. The core nodes `fmnext-n1`, `fmnext-n2`, `fmnext-n3` and the runner `fmnext-r1` belong to **`jett`**. Before this change they belonged to the `fmnext` admin account, which the cutover brought into production.

## Where node ownership lives

| Place | Field | Written by |
|---|---|---|
| `nodes` row of the node account (encrypted `data`) | `owner` | `_link_node_to_user` (entry.py), the only writer |
| `nodes` row, plaintext column | `user_bi` = blind index of `owner` | `_mirror_account_identity_tables`, when `_save_account` saves the record |
| owner's `users` row (encrypted `data`) | `nodes` list | `_link_node_to_user`, which also removes the node from the previous owner's list |
| `runner_nodes` | `registered_by` (and `allowed_owners`, which lists the owners whose jobs the runner takes) | `POST /api/runners/register`, same registrant only |

The following hold **no owner**:

- `mirror_https_endpoints`. Its `registration_sig` signs name, base URL, public key and time, never the owner.
- `cluster_members`.
- The catalog records. Their `owner` is the node or repository name.
- `org_repos` (`node_owner` is a node name).
- `role_grants` and `org_members`. `fmnext` had none.

"Served by fmnext-nN" on repository pages is the node name, so it does not change.

A node account's Ed25519 key signs its own requests. It is not bound to the owner, so the transfer re-signed nothing.

### What the UI shows

- **Desktop Network > Nodes (node rows and their repository copies):** the owner is the catalog `ownerUser` from `/api/repo/<o>/<r>/mirrors`, filled by `_mirror_owner_users` from the node record's `owner`. It is memoised for 5 minutes per app process.
- **Web dashboard:** shows no node owner at all. The catalog sanitizer never stores `ownerUser`.

## How the transfer was done

Run `tools/next_node_owner.py` (dry-run by default):

```sh
python3 tools/next_node_owner.py status
python3 tools/next_node_owner.py transfer --from fmnext --to jett --accept-key <jett's Ed25519 PEM> --execute
```

The new owner must accept. Use one of:

- `--accept-key PATH`: the target account's own signing key (its account `pubkey` or an enabled `owner_sign` device key). The key is read locally and never sent.
- `--session-env NAME`: an environment variable that holds a session token of the target account. It is sent as `Authorization: Bearer`.

Without either option, `--execute` refuses to run. The 2026-09-24 transfer below predates this requirement and ran on the old v1 flow.

**Cores.** This is the signed path, `POST /api/accounts/link-grant`.

- On the node, `python3 -I` signs `forkmesh-link-grant-v2\n<node>\n<target>\n<ts>\n<nonce>` with `/var/lib/forkmesh/identity/ed25519.pem`. The script arrives on stdin, and only the public key and signature come back. The key never leaves the node.
- The tool refuses the grant if that public key differs from the `nodes.pubkey` on record.
- The target accepts in the same request, with a `forkmesh-link-accept-v1` signature or its session.
- The app checks that the acceptor is the named target, burns the nonce, runs `_link_node_to_user`, and writes a `node.link_grant` audit row.

**Runner.** No API moves a runner's registrant. Re-register refuses another registrant with `name_taken`, and revoke keeps the row. The tool therefore runs one guarded rqlite transaction:

```sql
UPDATE runner_nodes SET registered_by=?, updated_at=? WHERE name=? AND registered_by=?
```

It does this only when all of these hold:

- the target is in `allowed_owners`;
- for a pool runner, the target is an admin;
- for a personal runner, `allowed_owners` is exactly the target.

**Audit.** Each change adds a `sensitive_audit_log` row: `node.owner_transfer` for each core and `runner.owner_transfer` for the runner, with details `{from, to, via}`.

**Idempotent.** Rerunning the tool reports "already owned" and writes nothing.

Result on 2026-09-24:

| Change | Rows |
|---|---|
| `nodes` rows (owner and `user_bi`) | 3 |
| `users` rows | 2: `fmnext.nodes` is now `[]`; `jett.nodes` gained the three cores |
| `runner_nodes` row | 1 |
| audit rows | 4 (ids 24721–24724) |

Runner routing did not change:

- A claim matches the job record's `organization` against `allowed_owners`, which is `["jett"]`.
- It matches the target node against `RUNNER_POOL_TARGET_NODES`.
- Neither depends on node ownership.

## A core with no node account (2026-10-13)

The daemon proves itself to the app only with its node key against `nodes.pubkey`; the `connectorToken` in `mirror-node.json` is the cloudflared tunnel token and is unused on cores. `fmnext-n1`..`n3` had accounts from before the cutover, and nothing in `tools/` created one, so `add-core` left `fmnext-n4` without an account: its catalog publications got HTTP 401, and the repository page listed only n2 and n3 under **Direct access** although n4 served the repository.

```sh
python3 tools/next_node_owner.py register            # dry-run: which cores lack an account
python3 tools/next_node_owner.py register n4 --execute
```

On the node, `python3 -I` signs `forkmesh-reserve-v1\n<node>\n<ts>` and `forkmesh-finalize-v1\n<node>\n\n<ts>` with `/var/lib/forkmesh/identity/ed25519.pem`; only the public key and the two signatures come back. The tool posts them to `/api/accounts/reserve` and `/api/accounts/finalize` with no link code and checks that `nodes.pubkey` reads back as the node's key. `add-core` now runs the same call in its `register` step. The new account has no owner; `transfer --from '' --to jett --accept-key <PEM> --execute` links it like the others (the owner is only the desktop's `ownerUser` label).

If an SSH call fails with `ControlPath too long` (an agent's long `TMPDIR`), set `FMNEXT_SSH_MUX_DIR` to a short directory, or to a path that cannot be created to turn multiplexing off.

## The `fmnext` account

It is **kept** as an admin service account, and only its ownership was removed. The account is still used by:

- the operator login probes in `docs/CUTOVER.md`;
- `cutover_data` (`config/next-admin.env`).

Security incidents ping every `is_admin=1` user, so both accounts receive IDS pings. The IDS e-mail recipient is `FMNEXT_IDS_ALERT_EMAIL`, not an account.

The rqlite user named `fmnext` in `deploy/fleet.toml` is a database credential, unrelated to the account.

Deleting the account needs the user's decision.

## Security note: link-grant consent (fixed 2026-09-24)

`_account_link_grant` used to take the new owner from the request body's `user`. As a result, anyone holding a node's key could re-home that node to any account.

Next now requires both the node's signed grant and the new owner's acceptance, either through its session or its own key. The owner is always the accepting account. The design, grant forms, errors and audit are in ForkMesh Next `docs/security/NODE_LINK_GRANT.md`.

## Version fields (for the desktop Network page)

There is one source: `desktop/CMakeLists.txt` `project(ForkMesh VERSION x.y.z)`, read by `next_common.release_version()`.

| Surface | Field | Value today |
|---|---|---|
| App `/api/version` | `version` (`APP_VERSION`, from `release.env` per release) | `1.2.4` |
| App `/api/version` | `rev` (`BUILD_REV`, the release git rev) | the deployed Next rev |
| Daemon `mirror-node.json` | `version` | `1.2.4` |

The daemon's `version` is stamped by the `next_fleet install --steps daemon` merge on every install. It reaches:

- the catalog record `version`;
- the mirrors API and network overview `version`;
- the runner status body;
- the `ForkMesh-Mirror-Node/<v>` User-Agent.

Desktop Nodes/Network should show:

- **Cores:** `version` from the catalog or roster (`/api/repo/<o>/<r>/mirrors` → `version`, owner → `ownerUser`).
- **App release:** `/api/version` `version` and `rev`.
- **Runner:** it publishes no catalog, and its version is in the daemon status only.

The daemon cannot report a build rev: the fleet build has no ldflags, and `--set-version` accepts only `x.y.z`.

The network overview previously showed a blank version, platform and commit for every node. Daemons send `updatedAt` as epoch milliseconds, which `Date.parse` rejected. `_catalog_updated_ms` now accepts it.
