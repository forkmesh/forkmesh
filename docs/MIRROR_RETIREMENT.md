# Retiring the production mirrors (mirror35–38)

On 2026-09-24 the four Vultr mirrors that served `forkmesh/forkmesh` before
the cutover (`mirror35`–`mirror38`, tag `forkmesh-mirror`) were retired. Every
role they had now runs on the cores (`fmnext-n1..3`) or the runner
(`fmnext-r1`). This page records where each role went and why.

## Where each role went

| Role | Before | Now |
|---|---|---|
| Git clone/fetch of `forkmesh/forkmesh` | the app answered 307 to a mirror or a core | the app answers 307 to a core only. The org alias `forkmesh/forkmesh` is backed by `fmnext-n1` (`org_repos.node_owner`) |
| New commits entering the fleet | the flagship's SSH fan-out (`forkmesh-mirror-fanout.sh`, root SSH + `runuser … git-receive-pack`) to each mirror | the headless publisher's signed HTTPS push through `app.forkmesh.com` (production repo, `docs/tools/source-publisher.md`) to a core gateway. See "Publishing to the cores" |
| Release blobs (`forkmesh-releases/sha256/…`) | the release host seeded each mirror over root SSH | the release host seeds the cores listed in `~/projects/forkmesh/config/fanout/release-hosts` with the fleet key (root writes files only, no `forkmesh-node` process) |
| Web-started agent runs | a headless mirror (`mirror35`, Claude Code) claimed jobs queued under its node name | owner jobs on a core-backed repository are queued on that core (`RUNNER_POOL_TARGET_NODES`) and claimed by the pool runner `fmnext-r1`. See "Agents" |
| Web merges | any group node advertising `merge-pull` | the cores (they always ran the merge executor) |
| `ssh.forkmesh.com` (read-only SSH clone) | round-robin A records over mirror35–37 | **retired**. See "SSH gateway" |
| Status page mirror rows, Mirrors tab | `mirror*` endpoint rows | the mirrors' rows are deleted, and the names are in `STATUS_RETIRED_MIRRORS` |
| Desktop Hosts list, autoscaler | four saved hosts, autoscaler on with a target of 3 | hosts removed and tombstoned in `hosts/decommissionedNodes`, autoscaler off |

## Publishing to the cores

The publisher pushes `refs/heads/*` and `refs/tags/*` (never forced, never
deleting) with the flagship desktop's identity key, signed per request
(`forkmesh-git-push-v1`). The server side:

1. **Authorization.** The org alias rewrites `/forkmesh/forkmesh` to
   `fmnext-n1/forkmesh`. The key belongs to `jett`, an owner of the
   `forkmesh` organization, so `_git_push_authorize` accepts it through the
   organization write path. A key that sits in the backing node's owner
   signing set but lacks `publish_repo` now falls through to that same check
   instead of being refused outright; that refusal was the `401
   push_unauthorized` the publisher saw while the alias pointed at `mirror35`.
2. **Only organization owners reach a core.** Since `docs/CORE_PUSH.md`, a
   push to a repository in a core node's namespace is accepted only from an
   owner of the linked organization (`_core_push_permitted`), by signed key
   or by the session-bound Basic credential. Other organization roles get
   `403 push_forbidden`. `FLAGSHIP_PUSH_PUBKEYS` was removed; the publisher
   is authorized as `jett`. Hosted repositories are routed before this check
   and are unaffected.
3. **A writable gateway entry.** `[app] flagship_receive_pack = true` in
   `deploy/fleet.toml` makes `next_fleet.py install nN --steps daemon` add
   `git-receive-pack` to the cores' `forkmesh/forkmesh` gateway entry
   (`false` removes it). The gateway's receive rules apply: fast-forward only,
   no deletes, a tag moves only by an owner's tag move, fsck, hooks never
   run. The receive runs inside the gateway,
   which is part of `forkmesh-mirror-node.service`, so the IDS sees no
   service-user process outside its unit. A writable entry has no immutable
   bootstrap pin; the app still routes it only by its signed proof.
4. **The org alias is the gateway-facing owner.** Gateways serve the
   repository only as `forkmesh/forkmesh`; nothing answers for a node
   namespace such as `fmnext-n1/forkmesh`. The push path now does what reads
   already did: it recovers the alias from the original URL
   (`_git_push_alias_owner`, verified against `org_repos`), asks each core for
   its proof as `routeOwner = forkmesh`, prefers the nodes whose stored
   advertisement carries `git-receive-pack` inside the four-endpoint failover
   window, and addresses the gateway at `/v1/repositories/forkmesh/forkmesh/…`.
   A signature over the alias owner (what a client pushing to
   `/forkmesh/forkmesh.git` signs) is accepted for exactly that alias.
5. **The new state becomes routable.** After an accepted owner push to a
   core, `_admit_core_push_state` fetches that core's freshly signed
   repository proof and records its refs digest in `repo_state_history`,
   exactly like an owner web merge. It then sends the `commits` wake to the
   group. The other cores fetch from the pushed core through the app (their
   upstream is `https://app.forkmesh.com/forkmesh/forkmesh`, which never
   routes a node to itself) and converge fast-forward only. Once two cores
   agree, the flagship quorum carries the state as before.

**The desktop's update clones through the alias too (2026-09-30).** The
desktop's Update, rebuild & restart asked `/api/install-source` for a node and
set the checkout's `origin` to `https://app.forkmesh.com/<node>/forkmesh`
(`resolveInstallCloneUrl`, `desktop/src/MainWindowSetup.cpp`). With the cores
that is `fmnext-n1/forkmesh`, which no gateway serves, so every `git fetch`
and the update's own `git pull` answered `503 {"error":"mirror_unavailable"}`.
It now uses `/api/install-source` only to confirm a mirror is online and sets
`origin` to `https://app.forkmesh.com/forkmesh/forkmesh`, the route
`install.sh` already used.

The SSH fan-out has no targets left (`config/fanout/hosts` holds only a
comment), so the publisher's step 2 is a no-op and step 3 does the work
through `config/fanout/gateways`:

```
cores https://app.forkmesh.com/forkmesh/forkmesh.git
```

## Agents

- An organization owner of the alias may act as the owner of its backing
  node for that repository only (`_org_owner_of_linked_repo`): owner-scope
  agents, the About editor, and fediverse repository settings. This mirrors
  the existing rule for web merges.
- `_org_agent_owner_targets` adds a `runner-pool` target when the
  repository's node is in `RUNNER_POOL_TARGET_NODES` and an active pool
  runner lists the **requesting account** in `allowed_owners` with a signed-in
  CLI (`runner_availability`). The queued job's `organization` is that
  account, so the runner's opt-in check is made against the person whose
  provider login the job uses. `fmnext-r1` lists only `jett`; the second
  organization owner cannot borrow that login.
- Since 2026-09-29 that target is listed under the **runner's** name
  (`node: "fmnext-r1"`, `queueNode: "fmnext-n1"`), because the composers and
  the session list said "Sent to fmnext-n1" for work `fmnext-r1` ran. The job
  row is still queued on the core (`org_agent_jobs.target_node`), which is
  what pool runners claim; the decision carries `runnerNode`, the session
  record stores it as `runnerNode`, and the session projection's
  `targetNode` shows it. A client that still sends the core's name as
  `targetNode` gets the same pool target. Sessions queued before the change
  keep showing the core.
- Runner leases address the core's gateway the same way: when the target
  node backs exactly one org alias for the repository (`org_repos` joined to
  `orgs`), the clone URL, `repositoryOwner` and both runner grants use that
  alias (`_gateway_owner` in `app/src/runner_leases.py`).
- The runner daemon now has `CAP_SETUID CAP_SETGID` in its bounding and
  ambient sets. `systemd-run --scope --uid/--gid` switches user inside the
  daemon's own process, and systemd leaves `CAP_SETUID` out of a root unit's
  permitted set unless it is ambient. Before this every job on `fmnext-r1`
  failed with `Failed to change GID/UID … Operation not permitted`.
- The desktop's "Continue in the cloud" handoff still needs a headless node
  in the account's own namespace; it does not use the pool.

## SSH gateway

`ssh.forkmesh.com` is retired rather than moved:

- The cores' only sshd is the operator's. The IDS locks a core down on any
  login with a key outside `ssh_fleet_fingerprints`, so public `git@` logins
  cannot share it.
- A second sshd would need its own port in nftables and the IDS listener
  list, a way for the IDS to tell its logins apart, and the gateway token on
  the cores. Measured use over 14 days was three logins per mirror (our own
  checks).
- Clone over HTTPS (`https://forkmesh.com/forkmesh/forkmesh`) is unaffected.
  The `SSH_GATEWAY_*` variables are out of the vault, so the app no longer
  advertises an `ssh://` URL, and the `ssh.forkmesh.com` A records are
  deleted.

## Traps met during the retirement

- **The gateway refuses forced updates and deletes**, so the publisher's
  HTTPS push cannot move a rewritten branch. Such a branch stays at its old
  value on the cores and the publisher lists it as `stale` without failing
  the pass. A moved tag (the release host moves `vX.Y.Z` tags) now goes
  through an audited owner tag move (`docs/CORE_PUSH.md`); that is how the
  cores' stale `v1.2.4` was corrected.
- **Empty loose ref files stop a core's upstream sync.** `fmnext-n1` had 14
  zero-byte files under `refs/heads` (all written at 17:44 UTC), and every
  fetch failed with `fatal: bad object refs/heads/…`. Removing only the empty
  files let git fall back to `packed-refs`, and the next full sync restored
  the tips. Find them with `find refs -type f -empty` in the bare repository.
- **A core's periodic cycle does not fetch.** Upstream fetches run on a
  `commits` event, the 30-minute safety resync, or
  `POST http://127.0.0.1:8791/v1/control/sync` on the node (`?local=1` skips
  the fetch).
- **`next_deploy` can leave a node half-activated.** Twice (n2, then n3) the
  activation script's SSH session ended after the `current` link moved but
  before the app restarted; the tool reported a failed gate and "rolled back"
  while the node kept serving the old release. `next_deploy.py restart --rev
  <rev> nN --execute` finished it both times.
- **Mirrors synced from each other.** `mirror35` fetched from `mirror36`,
  and `mirror36` from `mirror37`, every 15 s, so a mirror's gateway kept
  logging git requests after it left routing. That was peer sync, not
  visitors.

## Retired instances and final backups

Each mirror left routing through the admin node delete
(`POST /api/world/admin/nodes/delete`), showed no visitor requests for four
minutes afterwards (the only git traffic left was the next mirror's peer
sync), had its services stopped and its final backup taken, and was then
destroyed through the Vultr API, matched by id, label, IP and the
`forkmesh-mirror` tag. Its `mirrorNN.forkmesh.com` A record was deleted.
Before each delete, the node's finished agent sessions were re-pointed to
`fmnext-n1` so the admin delete would not drop them from the owner's list.

| Mirror | Vultr instance |
|---|---|
| mirror38 | `b85aa904-8b36-4488-bb12-b548e283a9cc` |
| mirror37 | `319d90b7-d790-4930-b6a9-503f5e0285e3` |
| mirror36 | `1e3370e2-f6b1-485e-b28a-9658caeaefb2` |
| mirror35 | `e693bb98-a8a2-43f7-a882-421728aa502b` |

Backups are in `~/projects/forkmesh-backup/data/mirror-final-backups/<name>/`
(directories 0700, files 0600):

- `repo-forkmesh-forkmesh.refs`: every ref and its object id.
- `repo-forkmesh-forkmesh.bundle`: the objects reachable from the mirror's
  refs that the flagship bare mirror
  (`~/projects/forkmesh/data/mirrors/forkmesh-forkmesh.git`) did not have. It
  verifies against that bare mirror (`git bundle verify`). When nothing was
  missing, `files.txt` says `empty` and there is no bundle.
- `agent-work/<id>.{bundle,refs,status,diff}` (mirror35): the same for each
  of the 22 web agent work trees.
- `state.tar.age`: `/var/lib/forkmesh` without the repository and work
  trees, plus `/etc/forkmesh*`, `/etc/caddy`, the systemd units and sshd
  drop-ins, encrypted to `config/backup-age.pub`. The Claude login in
  `/var/lib/forkmesh/.claude/.credentials.json` was left out.
- `db-rows.json`: the node's `nodes`, `mirror_https_endpoints`,
  `org_agent_sessions` and `org_agent_jobs` rows as stored (encrypted
  columns stay encrypted).
- `remote.sha256`: checksums taken on the mirror; every copied file was
  checked against them.

`mirror17-records/db-rows.json` holds the rows of the long-destroyed
`mirror17`, whose stale endpoint and node rows were removed the same day.

## Old tunnel mirrors (mirror2–34)

The volunteer and managed mirrors from before Next (`mirror2`, `mirror3`,
`mirror6`–`mirror9`, `mirror20`–`mirror34`) published `mirrorNN.forkmesh.com`
as proxied CNAMEs to their Cloudflare Tunnels
(`tools/cloudflare_tunnel_bootstrap.py`). On 2026-09-27 all 21 tunnels were
down with no connections and every name answered 530, so the 21 records
were deleted from the forkmesh.com zone. The records as they were (id, name,
target, comment) are saved in
`~/projects/forkmesh/data/fleet/retired-mirror-tunnel-dns-2026-09-27.json`.
The mirror machines behind them are deleted. The tunnel objects are still in
the Cloudflare account, inactive.

`FLAGSHIP_RECOVERY_NODE` names a node whose owning account may pin a new
flagship state through its newest endpoint when the two-mirror quorum
cannot (`_https_mirror_flagship_recovery_refs` in `app/src/entry.py`).
Production set it to `mirror29` in `deploy/fleet.toml [production.vars]`.
That node no longer has a `nodes` row, so the anchor fell back to an exact
`mirror29` digest, and only `fmnext-n1..3` have endpoints: the setting
already did nothing. It is removed from `[production.vars]`, so the app
uses the `app/wrangler.toml` default (empty, recovery off). The cores keep
`FORKMESH_VAR_FLAGSHIP_RECOVERY_NODE=mirror29` in `app.env` until their next
`next_fleet.py install nN --steps files,start`. The flagship now advances
only through the cores' quorum (two cores agreeing) or an owner push or
merge (see "Publishing to the cores").
