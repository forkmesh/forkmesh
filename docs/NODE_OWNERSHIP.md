# Fleet node ownership and version fields

Each core and runner has a node account that belongs to an operator account.
`tools/next_node_owner.py` inspects, provisions and transfers ownership
(dry-run by default).

## Where ownership lives

| Place | Field | Written by |
|---|---|---|
| `nodes` row (encrypted `data`) | `owner` | `_link_node_to_user` (`app/src/entry.py`), the only writer |
| `nodes` row, plaintext | `user_bi` = blind index of `owner` | `_mirror_account_identity_tables` when the account is saved |
| owner's `users` row | `nodes` list | `_link_node_to_user` (also removes the node from the previous owner) |
| `runner_nodes` | `registered_by`, `allowed_owners` | `POST /api/runners/register` |

`mirror_https_endpoints`, `cluster_members`, catalog records, `org_repos`,
role grants and organization members carry no node owner. A node account's
Ed25519 key signs its own requests and is not bound to the owner, so a
transfer re-signs nothing.

## Transferring ownership

```sh
python3 tools/next_node_owner.py status
python3 tools/next_node_owner.py transfer --from <old> --to <new> --accept-key <new owner's Ed25519 PEM> --execute
```

The new owner must accept, with `--accept-key PATH` (its own signing key, read
locally) or `--session-env NAME` (a session token sent as a bearer);
otherwise `--execute` refuses.

- **Cores** use the signed link grant ([security/NODE_LINK_GRANT.md](security/NODE_LINK_GRANT.md)):
  the node signs `forkmesh-link-grant-v2\n<node>\n<target>\n<ts>\n<nonce>`
  with its identity key (the script arrives on stdin; only the public key and
  signature leave the node), the tool checks the public key against
  `nodes.pubkey`, and the target accepts in the same request.
- **Runners** have no API to change the registrant, so the tool runs one
  guarded update (`UPDATE runner_nodes SET registered_by=? … WHERE name=? AND registered_by=?`)
  only when the target is in `allowed_owners` (and an admin for a pool
  runner, or the sole allowed owner for a personal runner).
- Each change writes `node.owner_transfer` or `runner.owner_transfer` to the
  sensitive audit log. Re-running reports "already owned".

## Provisioning a core's node account

The daemon authenticates to the app only with its node key against
`nodes.pubkey`.

```sh
python3 tools/next_node_owner.py register            # dry run for every core
python3 tools/next_node_owner.py register nN --execute
```

For each core it creates a missing account (the node signs
`forkmesh-reserve-v1` and `forkmesh-finalize-v1` with its identity key),
refuses an existing account bound to another key (a rebuilt core reusing a
name gets HTTP 401 until the owner reclaims the name), and links an unowned
core to the account that owns the other cores through the signed link grant.
`add-core` runs the same `provision_core` in its `register` step.

## Service accounts

An admin service account without a mail domain may remain for operator
probes; mark such accounts as service accounts so they receive no mail.
Security incidents ping every admin.

## Version fields

One source: `project(ForkMesh VERSION x.y.z)` in `desktop/CMakeLists.txt`, read
by `next_common.release_version()`.

| Surface | Field |
|---|---|
| App `/api/version` | `version` (`APP_VERSION` from the release) and `rev` (`BUILD_REV`) |
| Daemon `mirror-node.json` | `version`, stamped by `next_fleet install --steps daemon` |

The daemon version reaches the catalog record, the mirrors API, the runner
status and the `ForkMesh-Mirror-Node/<v>` User-Agent. The daemon cannot report
a build revision. The desktop shows core versions from the catalog, the app
release from `/api/version`, and runner versions from daemon status only.
