# Repositories hosted on the core nodes

A user with no desktop node and no external forge account can create a
repository on the website and use it with plain `git` over HTTPS. Every core
keeps a full replica. Later the user can move the repository to their own
node.

Code: `app/src/hosted_git.py` (create, push routing, quotas, private reads),
`app/src/hosted_repos.py` (replica exchange, templates), migration
`0199_hosted_repositories`, daemon `server/hosted.go` and
`server/hosted_push_mirror.go`, the `restricted` visibility in
`tools/mirror_gateway.py`, and the create page `/dashboard/repos/new`. On a self-hosted instance the
operator command `instance-repo` creates and fills one from a repository on
the server through the same create helpers ([INSTANCES.md](INSTANCES.md)).

## What a user gets

**New repository** fields: name (`[A-Za-z0-9._-]`, at most 64 characters, not
starting with `.` or ending in `.git`), owner (self or an organization where
the user can maintain), description, visibility (public or private), README,
`.gitignore` template, license. A public `https://` URL on a host in
`HOSTED_IMPORT_HOSTS` imports a cloneable copy.

After creation the page shows the remote URL
`https://app.forkmesh.com/<owner>/<name>`, the commands, and a username and
password. The password is the session-bound push credential (`fmp1.…`): 7
days, never stored, revoked with the session, renewable with
`POST /api/hosted-repositories/push-credential`. It also clones private
repositories. There are no personal access tokens and no SSH access.

**Quotas** (`app/wrangler.toml [vars]`): `HOSTED_REPO_MAX_PER_ACCOUNT`
(default 10) and `HOSTED_REPO_MAX_BYTES` (default 200 MiB, compared with the
largest copy any core reports; over it a push answers
`413 repository_quota_exceeded`). A core reports `git count-objects -v`
`size` plus `size-pack` (loose and packed object bytes), not the directory
size, and [maintenance](#maintenance) keeps duplicate packs from counting
for long. Push bodies are streamed, up to
`FORKMESH_MAX_GIT_PUSH_BODY_BYTES` (256 MiB). The creating account must have a
verified email.

**Git rules** (gateway receive-pack): the default and protected branches
fast-forward only; other branches may be force-pushed by accounts that may
write them ([CORE_PUSH.md](CORE_PUSH.md#force-pushes)); no ref deletes, new
tags allowed but never moved, only `refs/heads/*` and `refs/tags/*`, objects
fsck-checked, no hooks, every accepted push audited per ref.

**Deleting branches.** The owner (or an organization owner or admin) deletes
branches on the repository's Branches page, one at a time or several at once
(`POST /api/repo/<owner>/<name>/branches/delete`, at most 200 per request).
The default branch and protected branches are refused; any name Git accepts
can be deleted. The request becomes a job that one background worker runs a
branch at a time: a router-signed request to the core's merge executor, which
moves the tip to `refs/forkmesh/trash/branch/` and deletes the branch in one
ref transaction, then the same on every other core, each audited as
`repository.branch_delete`. When the job ends the app notes the change like a
push, so the cores wake and a push mirror with prune drops the branches on
its next push, and updates the catalog's `branchCount`. Details: [app/branch-ops.md](app/branch-ops.md#deleting-branches).

## Design

```
browser ── POST /api/hosted-repositories ──► app (any core; rqlite)
                                             │ catalog record + hosted_repos row
                                             └─ "hosted" node event ──► every core daemon
core daemon ── POST /api/hosted-repositories/replicas (node-signed) ──► app
            ◄── desired set: {id, owner, name, visibility, pushedAt, primary?, init?, sources[]}
    ├─ create <root>/<id>.git + gateway entry ("public" | "restricted")
    ├─ primary + empty: deterministic first commit, or a bounded import
    └─ behind / empty: fetch from a peer with a router-signed read grant, fast-forward only
git push ─► app (Basic credential) ─► last pusher, else rendezvous-first live core
git clone ─► app ─► public: a core whose signed refs proof matches the pin
                    private: owner or organization reader, then a core via capability
```

- **State** lives in rqlite: `hosted_repos` (spec, `pushed_at`, `pushed_by`)
  and `hosted_repo_copies` (each node writes only its own rows).
- **Primary:** the rendezvous-first live core for the repository id. Only it
  writes the first commit (deterministic, so two would-be primaries agree) or
  runs an import.
- **Pushes** go to the last pusher when live, else rendezvous order.
- **Replicas** behind `pushed_at` fetch from the pusher first using a
  runner-format read grant (at most 1 h) and apply refs with the same
  fast-forward-only reconciliation as upstream sync.
- **Wake-ups:** the payload-free `hosted` node event after create, push or
  first pin; a 2-minute safety reconcile; `POST /v1/control/sync`.
- **Gateway config** is rewritten by the daemon, one entry per repository and
  per organization alias. Public entries also get web merge operations when
  the node runs the merge executor; restricted entries do not.
- **Routing pin:** public reads go only to cores whose signed proof matches
  the pinned `stateHash`, recorded after each push or web merge
  (`record_hosted_push_state`).
- **Catalog head:** the record's `branch` and `commit` (what the logo,
  banner, About and README lookups read) move with the pin. Whenever
  `record_hosted_push_state` pins a new `stateHash`, it reads the default
  branch tip from the hosting core with a router-signed `branches` request
  (`_hosted_head`). If that read fails, the pin is still written and the old
  commit stays until the next push. `_admit_core_push_state` calls it too,
  with the state it just verified and no notifications. That covers pushes
  that never pass through the push proxy, such as an Actions commit-back from
  the runner route (`runner_actions.report` → `admit_push_state`). Before this,
  only the push proxy wrote `stateHash` and only an instance import wrote
  `commit`. A self-hosted instance whose workflows commit every few minutes
  kept a commit hours old, so its logo and About lookups read stale files.
  Tests: `test_a_new_pin_moves_the_catalog_head_to_the_default_branch_tip` and
  the tests after it in `app/tests/test_hosted_repositories.py`,
  `test_an_admitted_core_push_moves_the_hosted_catalog_head`.
- **Online badge:** the catalog's `liveHost` is normally "the record owner's
  node has a fresh, verified HTTPS endpoint". A hosted record is owned by a
  user account (`alice/project`), never by a node, so a hosted repository always
  read *offline* on the dashboard and repository pages, even while the core
  beside it served it and the instance's own mirror read *online*. The
  catalog (`catalog_routes.catalog_handler`) now also marks a `hosted-web`
  record live when a core that reported a non-empty copy in
  `hosted_repo_copies` has a fresh verified endpoint. Tests:
  `test_hosted_repository_is_online_while_a_live_core_serves_its_copy` and
  the two after it in `app/tests/test_catalog_signed_endpoint_liveness.py`.

- **Commit count and activity:** a node-published record carries
  `commitCount`, `mergeCount` and the 52-week activity series from
  `server/catalog.go`; a hosted record is written by the app, so it had none
  (a web-created repository) or a count capped at 10,000 by `instance-repo`.
  Each core now adds the stats of its copy to the replica report
  (`hostedCopyState`: `commit`, `commitCount`, `mergeCount`,
  `activityWeeks`, `mergeActivityWeeks`). The full `git rev-list --count` and
  the 52-week walk run once per default-branch tip per UTC day
  (`hostedReplicator.historyStats`) and are reused on every other exchange.
  The app (`hosted_repos.clean_stats`, `hosted_git.record_hosted_stats`)
  writes them into the catalog record when the reporting copy's routing digest
  is the pinned `stateHash` and its tip is the record's `commit`, so a copy
  that is behind never overwrites the counts. Writes happen only when a value
  changed, at most once per copy refresh (`COPY_REFRESH_MS`). This is a
  `server/` change: a node only reports stats after its binary is rebuilt.
  Until then, rerunning `instance-repo` refreshes an imported repository.
  Tests: `test_a_current_copy_reports_the_true_commit_count_and_activity` and
  the two after it in `app/tests/test_hosted_core.py`,
  `test_hosted_stats_update_only_the_record_whose_pin_they_describe` in
  `app/tests/test_hosted_repositories.py`,
  `TestHostedCopyStateReportsTheWholeHistoryCachedPerTip`.
- **Attested state after intake and merges:** the router checks clones
  against the routing digest (default branch plus tags,
  `routingRefsSHA256`), and pushes already pinned that. The collaboration
  intake acknowledgement (`server/intake.go`) and merge results
  (`server/merge.go`) recorded the all-branches digest instead, so on a
  repository with more than one branch they added a pin no node could match.
  Both now attest the routing digest; the gateway's own generation pin still
  uses the full digest. Test: the merge executor test in
  `server/merge_test.go` compares the attested state with the live routing
  digest.

### Private repositories

A private repository is a `restricted` gateway entry reachable only by a
router capability or runner grant; public routes, dashboards and counts never
see it. The app serves Git and `/api/repo/...` reads through
`hosted_private_read` to the owner or organization readers; others get `401`
(Git) or `404` (web). This is access control, not end-to-end encryption: the
cores hold plaintext. Owner-sealed repositories need the owner's own node.

### Organizations

The repository is stored under the creator's namespace, linked in `org_repos`
so `/<org>/<name>` works, and pushes use organization roles (write or better;
private reads need read or better).

`org_repos.repo` is always the lowercase name: every reader (the git and API
alias rewrite, org read and push permissions) looks it up lowercased, and the
organization link endpoint stores it that way. Hosted creation used to store
the name as typed, so a mixed-case repository such as `Bolt` had a link no
lookup could find: `/<org>/Bolt.git` and the short `/Bolt.git` answered
`not_found` and organization members other than the creator could not push.
`link_organization` now stores the lowercase name and renames an existing
mixed-case link of the same repository, and re-running `instance-repo` on an
existing repository relinks it, which repairs a link written before the fix.

### Imports

The primary fetches heads and tags from an allowed `https` host (no
credentials, port or query), stops past the size limit or 10 minutes, and
points HEAD at `main`, `master`, `trunk` or `develop`. Private sources are not
supported. The separate metadata importer (`app/src/repository_imports.py`)
imports issues, pulls and releases only.

### Contributor pushes

Without a node of their own, a signed-in contributor has no fork to push to,
so a pull request against a hosted repository was out of reach. An operator
can open named public repositories to contributor pushes
(`HOSTED_GIT_CONTRIBUTOR_PUSH`, user guide in
`www/docs/hosted-repositories.md`): any active, email-verified user account
may then push branches under `refs/heads/pr/<account>/` of the repository
itself (the namespace the SSH gateway's pull-request grant uses), with the
same session-bound credential an owner uses.

- **Allowlist** (`hosted_git`): `*`, `owner/*` or `owner/repo` items,
  separated by whitespace or commas, case-insensitive; a route alias or its
  backing node matches. Unparseable entries are ignored, so a typo narrows
  access rather than widening it. The repository must be listed and public,
  and the account an active, email-verified user; every check fails closed
  and is repeated on each push.
- **Credential.** `POST /api/hosted-repositories/push-credential` mints the
  session-bound Basic credential. Owners and writers get full access; a
  contributor gets one limited to its namespace, and the push answer names
  that one ref namespace (an owner or writer gets no prefix). Signing out
  revokes it.
- **Enforcement.** The routing Worker signs `pushPrefix` into the
  receive-pack capability, so a contributor cannot widen it, and the gateway
  (`tools/mirror_gateway.py`) refuses any command outside it, with at most
  `MAX_CONTRIBUTOR_BRANCHES` (32) branches there. A refusal is answered as a
  receive-pack report (`ng <ref> <reason>`), so stock Git prints the reason
  beside each ref instead of an opaque HTTP failure. Deletes and force pushes
  are refused for everyone on this path, so only a maintainer removes a
  contributor's merged branches. Only new objects cross the wire.
- **No workflows.** A contributor's push runs nobody's workflows
  (`core_push`): `on: push` would execute untrusted code with the
  repository's runner access.
- **No quota.** There is no per-account byte quota or rate limit on this
  path beyond the gateway's `receive.maxInputSize` per push.

## Forks

**Forks → Fork this repository** on a public hosted repository creates
`<you>/<name>`, a hosted repository you own, marked `forkedFrom` the parent
and listed on its forks (`repo_forks`). Code: `hosted_git.hosted_fork_create`
(reached from `POST /api/repo/<owner>/<repo>/forks` before the managed-mirror
path), `hosted_repos.fork_of`, `server/hosted_forks.go`.

- **Storage.** The hosted record carries `fork: {of, network}`; the replica
  exchange passes `forkOf` and `network` (the root repository's id) to every
  core. Each core builds the fork from its own copy of the parent: the
  network pool `<root>/.pools/<network>.git` is a `git clone --local` of the
  root (hard links, so no extra disk until the parent repacks), refreshed
  from the parent into `refs/pools/<parent>/…` before each new fork, and the
  fork's `objects/info/alternates` points at it. Refs are copied from the
  parent with nothing transferred. A fork costs about 26 KB (measured in
  `TestHostedForkBorrowsFromTheNetworkPool`); its own pushes stay in it.
- **One core picks the refs.** Only the fork's primary core copies the
  parent's heads, tags and HEAD into a new fork. Every other core sets up the
  pool and the alternates and then fetches the fork's refs from its peers,
  which transfers almost nothing, so all cores agree on the fork's refs (and
  refs digest) even when their copies of the parent differ for a moment
  (`TestHostedForkRefsComeFromThePrimaryOnly`).
- **Parent still public.** The replica exchange names `forkOf` only while the
  parent is a live public hosted repository, and the core also refuses to
  pool a parent its own answer lists as private. A parent made private after
  the fork never feeds a fork that a core builds later (a new core, or a
  wiped directory), nor the shared pool
  (`test_a_fork_of_a_parent_made_private_is_not_built_from_it`,
  `TestHostedForkOfAPrivateParentIsNotPooled`).
- **Built under the parent's lock.** Creating and refreshing the pool and
  copying the fork's refs hold a shared lock on the parent's
  `forkmesh-maintenance.lock`, so a geometric repack cannot delete objects
  mid-clone; when maintenance holds it, the fork is built on the next cycle.
  A new pool must pass `git fsck --connectivity-only` before it is renamed
  into place.
- **Never pruned.** The pool is not served and not maintained (maintenance
  walks only `<root>/hr*.git`, and repacks forks with `-l`), so a
  force-push, repack or deletion of the parent never removes an object a
  fork needs. An operator may delete a pool only when no fork in its network
  is left.
- **Limits.** Public parents only (shared objects would leak across a
  private network); active users with a verified email; one fork per
  account per parent (a second click links to the first);
  `HOSTED_MAX_FORKS_PER_ACCOUNT` (default 20), separate from the repository
  quota. Forks of forks join the root's network.
- **Pull requests.** The composer on a fork targets the parent
  (`forkedFrom`) and sends a patch-backed pull request (patch and commits),
  so merging into the parent needs no fetch from the fork.
- Contributors who would rather not fork can push `pr/<account>/` branches
  where `HOSTED_GIT_CONTRIBUTOR_PUSH` allows it.

## Moving to your own node

When the owner's node publishes the repository through the signed catalog,
the hosted record is replaced; the next exchange marks the row deleted, frees
the quota, stops routing pushes and moves core copies to `.trash`. Steps:
`git clone --mirror`, add it on the node, publish from the node.

## Push mirrors

The owner can mirror a hosted repository to GitHub or GitLab with a personal
access token. Only the primary pushes. The exchange carries an optional
`pushMirror` (`revision`, `provider`, `url`, `username`, `token`, `prune`,
`refs`) to the primary, and the daemon reports `pushMirrors` results
(`revision`, `digest`, `ok`, `at`, `error`, and `refs`, the count of updated
refs).

- Valid only for `github`/`gitlab`, URL exactly
  `https://<host>/<owner>/<name>.git` on `hosted.pushHosts`, a token of at most
  512 characters without whitespace.
- Push when settings change, when refs change, or every 6 hours; retry
  failures after 10 minutes; each push is limited to 10 minutes.
- `refs` is `all` (the default, also when absent) or `default`. With `all`:
  `git push --porcelain [--prune] <url> +refs/heads/*:refs/heads/* +refs/tags/*:refs/tags/*`.
  With `default`, only the branch the hosted repository's `HEAD` points at,
  plus tags: `git push --porcelain [--prune] <url> +refs/heads/<head>:refs/heads/<head> +refs/tags/*:refs/tags/*`.
  `--prune` alone never deletes branches outside that refspec, so with prune
  the daemon first runs `git ls-remote --heads <url>` (same header, timeout
  and redaction) and adds `:refs/heads/<name>` for every other branch on the
  target to the same push (at most 1000; names git would refuse are skipped).
  In `default` mode only the head and tags count as a change, so pushes to
  other branches do not trigger a push. Changing `refs` triggers a push even
  with the same `revision`. The owner sets it in the mirror form ("Branches")
  or with `refs` in the `save` action of the provider-mirror API.
- Both commands run with the token only in an `http.extraHeader` passed
  through `GIT_CONFIG_*` environment variables, no credential helper, no
  redirects, HTTPS only.
  Errors are redacted (token and header value replaced) and folded to 300
  characters.
- A branch deleted on the Branches page changes the refs digest, so it
  triggers a push, and with prune the branch is removed from the target.

## Operations

- Root on each core: `/var/lib/forkmesh/hosted/<id>.git` (owned by the daemon
  user, 0700). Removed repositories go to `.trash/<id>-<unix>.git`; nothing
  deletes them automatically. Empty it by exact name after the retention
  window.
- Logs: `journalctl -u forkmesh-mirror-node | grep hosted`.
- Database: `hosted_repos`, `hosted_repo_copies`.
- Watch disk use; lower the per-account quota if needed.
- `deploy/core/render.py` writes `FORKMESH_VAR_HOSTED_GIT_CORE_NODES` (every
  core, or empty with `[hosted] enabled = false`) and
  `FORKMESH_VAR_HOSTED_GIT_CORE_OWNERS` (plus `[hosted] retired_cores`) into
  `app.env`. An empty list makes the create form fall back to the node flow.
- Rollout order: node files (`files,start`), then daemon, then the app
  release, one core at a time.

## Maintenance

Every accepted push (web, `git push` through the app, Actions commit-back
pushes on the runner route `/v1/runner/<owner>/<repo>.git`) is received by
`tools/mirror_gateway.py` with `receive.unpackLimit=0`, so each one adds a
pack, and replica fetches add more. Nothing compacted hosted repositories
before 2026-10-17: git's own `gc --auto` waits for 50 packs, and on the Bolt
instance the size check tripped on duplicate packs first. The git node now
maintains them (`server/hosted_maintenance.go`):

- **Trigger.** After a push that moved refs the gateway touches
  `<id>.git/forkmesh-maintenance-due` (only in a repository that has the
  maintenance lock file, which the node creates, so configured mirrors are
  left alone). `instance-repo` touches it after a fill. A goroutine of the
  node daemon looks for the marker every minute, and every 15 minutes also
  scans each hosted repository for more than 4 packs or 256 loose objects.
  The work never runs on the request path.
- **What runs.** `git repack -d -l -q --geometric=2`: small packs and loose
  objects are rolled up while the large pack stays untouched, and two packs
  of similar size (a duplicate history) are merged into one, each object
  written once. `-d` removes only packs and loose objects the new pack made
  redundant; nothing unreachable is pruned, so git's default grace is kept
  and a concurrent push never loses a base object
  ([destructive actions](engineering/destructive-actions.md)).
- **Low cost.** The command runs under `nice -n 19` and `ionice -c 3` when
  they exist, with `pack.threads=1`, `pack.windowMemory=64m`,
  `pack.deltaCacheSize=16m`, `core.deltaBaseCacheLimit=16m` and a 256 MiB
  pack window limit, so it fits beside the app on a 1 GB server, and is
  stopped after 30 minutes. The node also sets `receive.autogc=false` and
  `maintenance.auto=false` in each hosted repository, so neither a push nor a
  replica fetch starts an unbounded `gc --auto` of its own.
- **Lock.** `<id>.git/forkmesh-maintenance.lock`. The gateway holds a shared
  `flock` on it while `receive-pack` runs; maintenance takes it exclusively
  and non-blocking, so it never starts while a push is being received and
  retries on the next tick. A push that arrives during a repack waits for it,
  up to `MAINTENANCE_LOCK_WAIT_SECONDS` (120 s), and then proceeds: git's
  repack removes only the packs it rewrote, so a pack written meanwhile is
  safe. A repack that fails puts the marker back.
- **Replica fetches.** The fetch guard in `boundedGit` compares the stored
  object bytes at the start plus what the directory grew by during the fetch
  with `hosted.maxRepositoryBytes`, so old duplicate packs no longer stop a
  replica from catching up.
- **Logs.** `hosted maintenance <id>.git: <before> -> <after> bytes in <time>`
  in the node's journal.

## Known gaps

- `git push` cannot delete a branch or force-push; delete branches on the
  Branches page instead.
- A core offline longer than the removal window keeps an orphaned copy.
- Concurrent divergent pushes on two cores during failover converge to the
  newer commit.
