# Gateways: HTTPS and SSH

The daemon never serves Git itself. It supervises the Python HTTPS gateway and
leaves SSH writes to the SSH gateway and broker.

| Component | Code | Guide |
| --- | --- | --- |
| HTTPS gateway (read-only `upload-pack`, dashboard, merge/Actions couriers, `/v1/node/logs`) | `tools/mirror_gateway.py` | [direct-https-mirror-gateway.md](../../www/docs/operations/direct-https-mirror-gateway.md) |
| SSH gateway (push over SSH, forced command) | `tools/ssh_gateway.py`, `server/packaging/ssh/ssh-gateway-entrypoint`, `tools/provision_ssh_gateway.sh` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |
| SSH authorization broker (local authorization on independent mirrors) | `tools/ssh_authorization_broker.py`, `server/packaging/systemd/forkmesh-ssh-authorization-broker.service` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md), [breakout-authorization-replica.md](../../www/docs/operations/breakout-authorization-replica.md) |
| Post-receive refresh | `tools/ssh_post_receive_refresh.py`, `server/packaging/ssh/ssh-refresh-notify` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |
| SSH host certificates | `tools/sign_ssh_host_certs.sh` | [git-ssh-gateway.md](../../www/docs/operations/git-ssh-gateway.md) |

JSON schemas for these configs are in `www/docs/*.schema.json`
(`mirror-gateway-config`, `mirror-gateway-manifest`, `mirror-endpoint`,
`ssh-gateway-config`, `ssh-authorization-broker`, `ssh-post-receive-refresh`,
`mirror-actions-*`).

## Gateway generations

The daemon writes the gateway config and sends SIGHUP; the gateway acknowledges
right after the validated swap and warms caches afterwards in a background
thread. The daemon waits up to 90 s (`gatewayReloadTimeout`) and restarts only
a gateway that is not responding, after a random delay of up to 20 s, so a
slow reload never restarts the whole fleet at once.

## Traps

- An older gateway refuses to start on a config key it does not know. Deploy
  `tools/mirror_gateway.py` together with the daemon.
- A missing path is a repository answer (404 `not_found`), not an outage
  (503 `mirror_unavailable`). `git cat-file blob <commit>:<path>` exits 128
  both for an absent path and for a broken object store, so `blob`, `blobs`,
  `tree` and `raw` look the entry up with `ls-tree` on a literal pathspec and
  read the object by id.
- `tree` lists at most `MAX_TREE_ENTRIES` (500) entries of a code folder, with
  `truncated: true` past that. The collaboration folders
  (`COLLABORATION_TREE_ROOTS`: `.forkmesh/issues`, `pulls`, `discussions`,
  `releases`) list up to `MAX_COLLABORATION_TREE_ENTRIES` (50,000): they skip
  the per-entry history and analysis, so a long listing is cheap, and the
  issue list reads `.forkmesh/issues/open` as one listing. With the 500 cap a
  mirror of a forge with 547 open issues showed "Open 500" and never loaded
  the rest.
- The HTTPS gateway has no `receive-pack`. Pushes go through SSH or through the
  Worker's hosted-push path ([hosted-repositories.md](../../www/docs/hosted-repositories.md)).
- A search for a common word is a repository answer too, not an outage.
  `search` streams `git grep` and stops at `MAX_SEARCH_RESULTS` matches, at
  2 MB of output or at the process timeout, returning what it has with
  `truncated: true`. It answers 503 only when git fails before any match.
