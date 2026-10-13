# Owner pushes to the core nodes

The cores (`fmnext-n1..3`) hold `forkmesh/forkmesh` and any other repository
whose namespace is a core node (the org alias `forkmesh/forkmesh` is backed by
`fmnext-n1`). Owners push to them with plain `git push` over HTTPS:

```
git push https://app.forkmesh.com/forkmesh/forkmesh.git <branch>
```

The per-core names (`https://nN.next.forkmesh.com/forkmesh/forkmesh.git`)
serve the same app, so they take the same pushes under the same rules.

Code: `app/src/entry.py` (`_https_mirror_push_proxy`, `_core_push_permitted`,
`_admit_core_push_state`, `_ref_updates_from`), `app/src/core_push.py` (tag
moves, audit, hooks), `tools/mirror_gateway.py` (`enforce_push_ref_policy`,
`X-ForkMesh-Ref-Updates`), `server/gateway.go` (`adoptAdmittedTagMoves`),
migration `0205_git_tag_moves`. The publisher side is in the production repo,
`docs/tools/source-publisher.md`.

## Who can push

| Repository | May push |
|---|---|
| Linked to an organization (`org_repos`) | an **owner** of that organization. Admins, maintainers, members, auditors and guests are refused |
| Personal (a core node's namespace with no organization link) | the account that owns the node (`_account_owns_node`); the node account itself is refused |

Anyone else gets `403 push_forbidden` before any core is contacted, and the
refusal is audited (`git.push_authorize`, `denied`,
`reason = organization_owner_required`). An anonymous request gets the usual
`401` Basic challenge. For `forkmesh/forkmesh` today that means `jett` and
`mdkaifansari04`.

Credentials are the ones hosted repositories already use, nothing new:

- **Session-bound Basic credential** (`fmp1.…`, 7 days, dies with the
  browser session, never stored). Mint it with
  `POST /api/hosted-repositories/push-credential` and
  `{"owner": "forkmesh", "name": "forkmesh", "sessionToken": …}`. The org
  alias is resolved to the backing node, and for a core-held repository only
  the owners above get one. Username = account name, password = the
  credential.
- **Signed requests** (`forkmesh-git-push-v1`, `X-ForkMesh-Pubkey/-Ts/-Sig`)
  with the account's primary key or an enabled device key. The headless
  publisher on the deploy box uses this with the flagship identity key, which
  belongs to `jett`.

One more writer bypasses the owner rule: the developer docs page
(`/dashboard/docs`, [app/docs-book.md](app/docs-book.md)) lets an account with
`users.is_admin` commit one edited `docs/**/*.md` file to `main`. The App builds
that commit and sends it through the same `_https_mirror_push_send`, so the ref
rules below still apply.

There are no SSH pushes to the cores (the cores' sshd is the operator's; see
`docs/MIRROR_RETIREMENT.md`, "SSH gateway") and no personal access tokens.
`FLAGSHIP_PUSH_PUBKEYS` is gone: the publisher is authorized as `jett`, an
organization owner, like any other owner push.

## Ref rules

The gateway enforces these for every router-authorized push (hosted
repositories included); git runs only after the command list passed.

| Ref | Rule |
|---|---|
| `main` and every other branch | fast-forward only (`receive.denyNonFastForwards`); never deleted (`receive.denyDeletes`). The same for everyone, owners included |
| new tag | allowed |
| existing tag | moves only to the exact object named by a pending owner tag move (below); never deleted |
| anything outside `refs/heads/` and `refs/tags/` | refused |

**Why owners cannot force or delete branches.** It matches hosted
repositories, and the cores converge by fetching from each other
fast-forward only (`reconcileUpstreamRefs`): a deleted branch is fetched back
from a peer, and a rewound branch loses to the peer's newer commit. A rule the
fleet cannot replicate would leave the cores disagreeing. A rewritten branch
stays at the cores' value; push it under a new name.
A branch is deleted on the cores only through the executor, which trashes the
tip first so peers do not fetch it back ([Branches the source
deleted](#branches-the-source-deleted-2026-10-06)).

**Why tags need their own rule.** git's `receive.denyNonFastForwards` only
covers `refs/heads/`, so before this any authorized pusher could rewrite a
tag with `git push --force`.

### Moving a tag

A tag move is an explicit, audited owner action, separate from the push:

1. `POST /api/git/tag-moves` with `{"owner", "repo", "ref", "target"}`
   (`ref` may be `v1.2.4` or `refs/tags/v1.2.4`; `target` is the new object
   id, the tag object for an annotated tag). Authenticate with the session
   (`sessionToken` or `Authorization: Bearer`) or sign
   `forkmesh-tag-move-v1\n{owner}\n{repo}\n{ref}\n{target}\n{ts}` with the
   same headers as a signed push. Only accounts that may push the repository
   (above) are accepted, only on a core-held repository (`409
   tag_moves_unavailable` elsewhere). The row lives 15 minutes in
   `git_tag_moves`; the decision is audited as `git.tag_move_authorize`.
2. `git push --force origin refs/tags/v1.2.4` within 15 minutes, as the same
   account. The app puts that account's pending moves (at most 8) in the
   router-signed capability target (`?tagMoves=refs/tags/v1.2.4:<oid>`), and
   the gateway lets exactly those tags move to exactly those objects.
3. When the gateway reports the move, the row is marked used and the move is
   audited as `git.tag_move` with the old and new object ids.

The publisher does steps 1 and 2 by itself when a push is rejected with
`already exists` (it moved `v1.2.4` this way).

## Consistency

One source of ordering: the core that accepts a push. Its receive is
fast-forward only; nothing else writes those refs.

1. The app streams the push to one core (the org alias is the
   gateway-facing owner, as for reads).
2. `_admit_core_push_state` asks that core for its freshly signed repository
   proof and records its routing digest (default branch + tags) in
   `repo_state_history`, then sends the `commits` wake with that digest.
3. The other cores fetch from the pushed core through the app and converge
   fast-forward only. A new tag is created like any ref. A moved tag is
   adopted only when adopting it gives exactly the admitted digest from the
   wake (`adoptAdmittedTagMoves`); a stale peer can never move a tag.
4. The headless publisher on the deploy box pushes the source checkout the
   same way. When the cores' `main` holds commits the checkout lacks (an owner
   pushed), the push is rejected (`fetch first`), and the publisher fetches the
   cores' `main` into the checkout: a fast-forward when the checkout is behind,
   a merge commit when both moved and `git merge-tree` finds no conflict. Its
   next pass pushes the result, which is a fast-forward on the cores. A real
   conflict is logged as `DIVERGED … merge <sha> by hand` and nothing is
   changed. The desktop's own mesh convergence does the same fast-forward.

### Every core is woken at once (2026-10-04)

A merge or push that lands on one core must reach the other cores straight
away, not on their next timed sync. Three paths send the `commits` wake:
owner pushes (`_admit_core_push_state` in `app/src/entry.py`), web pull merges
(`app/src/route_handlers.py`) and agent-branch merges
(`app/src/agent_branches.py`). Two gaps let a core miss it:

- `notify_repo_mirrors` (`app/src/node_events_runtime.py`) skipped the
  repository's canonical owner, so the source is not woken by its own change.
  For `forkmesh/forkmesh` that owner is `fmnext-n1`, which is a core: a merge
  landed on n2 or n3 woke the other peer but left n1 on its timer. The source
  is now skipped only when it is not a core (`HOSTED_GIT_CORE_NODES`). When
  the source is a core, every core is a target even if it is missing from the
  catalog group. The core that took the change is woken too; its fetch finds
  nothing new.
- Each path sent the wake only when the signed state after the change was
  admitted. A failed proof, an unverified digest or a D1 error sent no wake at
  all. The wake now always goes out; without an admitted digest it carries no
  `state` and generation `0`, which still makes each core sync now
  (`server/daemon.go` treats every `commits` event as a sync request). A
  stateless frame is not stored for replay, so it cannot replace a pending
  target.

"Instant" is as fast as a core fetches from a peer through the app, usually a
few seconds. A lost wake still falls back to the timed sync. Tests:
`app/tests/test_node_event_push.py` (`test_a_merge_on_any_core_wakes_every_core_*`,
`test_a_core_push_wakes_the_cores_even_when_its_state_is_not_admitted`).

### Broken refs after an unclean halt (2026-09-25)

A Vultr halt of n2 left `refs/heads/429-sure-watch-feature` as an empty file.
git's connectivity check then failed every fetch n2 made. n2 also served the
zero id for that ref and answered its peers' wants with 404, so n1 failed
whenever its random peer was n2. On n3, about 200 empty refs sat in
`refs/forkmesh/upstream`, where `clearRefNamespace` never saw them because
`for-each-ref` skips broken refs. At the same time n1's avoid hint (`n1`)
never matched its endpoint record (`fmnext-n1`), so n1 could be routed to
itself and "sync" to its own stale state.

Fixes:

- `dropBrokenRefs` (`server/gateway.go`) walks the loose ref files before
  every fetch and after a failed one. It removes empty or malformed refs,
  refs whose object is missing (one `cat-file --batch-check`) and `.lock`
  files older than 10 minutes, and logs what it dropped. A peer supplies any
  real branch again on the next fetch.
- A failed fetch clears `refs/forkmesh/upstream` and retries up to twice. Each
  retry adds the peer git was redirected to (`redirecting to https://nN.`) to
  `X-ForkMesh-Avoid-Node`, which is now a comma-separated list of up to four
  names.
- The app (`_https_mirror_avoided_nodes`, `_https_mirror_record_avoided`)
  matches an avoided name against the node name or the first label of the
  endpoint's host, so `n1` excludes `fmnext-n1`.
- `broken_served_refs` (`tools/mirror_gateway.py`) makes `check_integrity`
  fail while a branch or tag ref is broken. The app then routes around the
  node until its own sync heals it.

### Branches the source deleted (2026-10-06)

A push never deletes, so before this the cores kept every branch ever pushed
(788 on 2026-10-06), merged and abandoned ones included, and the source's
mesh converge kept offering them back. Deleting a branch on the desktop now
leaves its tip in `refs/forkmesh/trash/deleted/<b>` in the source checkout
(`docs/desktop/README.md`). After each pass, outside the publish lock and in
its own unit (`forkmesh-mirror-branch-deletes.service`,
[source-publisher.md](tools/source-publisher.md#flow)), the publisher lists the cores'
branches and, for every tombstoned branch the checkout no longer has whose
cores tip the tombstone contains, asks the app to delete it:

```
POST /api/git/branch-deletes   {"owner", "repo", "branch", "expect"}
X-ForkMesh-Pubkey / -Ts / -Sig over
forkmesh-branch-delete-v1\n{owner}\n{repo}\n{branch}\n{expect}\n{ts}
```

`core_push.branch_deletes_handler` accepts the same keys and the same owners
as tag moves (`_core_push_permitted`; a browser session works too). It
refuses `main`, `master`, `HEAD` and `forkmesh/*`, then runs the Branches
page's delete on the cores (`_branch_op_on_cores`, `op: delete`): the first
core deletes the branch if it is still at `expect`, saving the tip under
`refs/forkmesh/trash/branch/`, and `_fan_out_cleanup` repeats it on the
others. Peer reconcile skips a tip that trash ref contains, so the delete
holds across the fleet. The audit action is `repository.branch_delete`.
A delete the executor runs as a job answers `202 processing` with
`requestId`, `node` and `pinBase`; the publisher posts again with those (the
same resume the Branches page does) every 2 s, up to 15 times, and only that
finishing call fans the delete out to the other cores. The publisher lists
branches on every core it reads (`core_read_urls`, from `deploy/fleet.toml`)
as well as the edge, so a branch that one core still holds counts as
pending; and a delete whose first core answers `branch_not_found` still fans
out to the rest (`core_branch_op`), so such a straggler is removed on the
next pass. The publisher sends
at most 20 per pass; a refused or unreachable delete is
logged and tried again on the next pass, and a cores tip past the tombstone
(someone pushed after the delete) is left alone.

### Listing the branches the cores deleted (2026-10-07)

The other direction: a branch deleted on the cores (the Branches page, the
publisher's pass above, **Merge & clean up**, an operator `delete-ref`) has to
leave the source checkout too, or the publisher pushes it straight back. The
cores keep every such tip under `refs/forkmesh/trash/branch/<b>/<token>` or
`refs/forkmesh/trash/agent-branch/<b>/<token>` (`branchTrashPrefixFor`,
`server/merge_branch.go`), but the gateway hides `refs/forkmesh/` from every
read, and showing it would keep a deleted branch's commits fetchable by any
reader, including a branch deleted because it held something it should not.
So the owners read the list through the app instead:

```
POST /api/git/branch-trash   {"owner", "repo"}
X-ForkMesh-Pubkey / -Ts / -Sig over
forkmesh-branch-trash-v1\n{owner}\n{repo}\n{ts}

200 {"ok": true, "owner", "repo",
     "entries": [{"branch", "oid"}, ...],   sorted by branch, then oid
     "truncated": false, "cores": ["fmnext-n1", ...],
     "unanswered": [...], "listedAt": <ms>}
```

- **Who.** `core_push.branch_trash_handler` takes the same signature scheme
  and the same owners as tag moves and branch deletes (`_signed_account`, the
  account's primary key or an enabled device key; or a browser session;
  `_core_push_permitted`). Anyone else gets `401`/`403`, and a refusal is
  audited (`repository.branch_trash_read`, `denied`); allowed reads are not,
  since the desktop reads once a minute. A repository the cores do not hold
  answers `409 branch_trash_unavailable`.
- **Where from.** `agent_branches.core_branch_trash` asks every core that
  runs the branch-delete executor (`_https_mirror_candidates`, operation
  `delete-agent-branch`) with a router-signed `GET
  /v1/repositories/<owner>/<repo>/branch-trash`
  (`_https_mirror_branch_trash_proxy`) and returns the union, since a core
  that missed a fan-out may hold less. Cores that did not answer are named in
  `unanswered`; none answering is `503 cores_unavailable`.
- **The node side.** `tools/mirror_gateway.py` serves `branch-trash` only
  behind a router capability, only for a repository that has
  `delete-agent-branch`, never on `/v1/public` or the `/api/repo` alias, and
  only as a `GET` with no query or body. It lists the two trash namespaces
  (`GitRepository.branch_trash`), drops anything that is not
  `<branch>/<16 hex>`, and stops at 5000 entries (`truncated`). The app
  checks the answer field by field (`_https_mirror_branch_trash_result`).
- **Bounds.** At most one round of core reads per repository every 30 s per
  app process (`BRANCH_TRASH_CACHE_MS`); repeats inside that window get the
  cached list. The per-IP edge limiter applies as for the routes beside it.
- **Time.** Neither the trash ref nor the executor records the delete time
  in a ref, so the list carries none.

The desktop's converge is the reader ([desktop/README.md](desktop/README.md),
"A deleted branch leaves its tip"): it deletes a local branch only when the
listing names it, the cores' read lacks it, no worktree has it, and its tip
is the listed tip or inside it; anything that fails or does not verify
deletes nothing.

### Removing a branch (operator)

A branch the source never had can still be removed by the operator on every
core, through the tooling:

```
python3 tools/next_fleet.py delete-ref nN --repo forkmesh/forkmesh \
    --ref refs/heads/<branch> --expect <sha> --execute
```

It tells the node's IDS first (allow processes, 5 minutes), refuses `main`,
`master`, `forkmesh/pulls` and anything outside `refs/heads/`, deletes only
when the branch is still at `--expect` and that commit is contained in
`main`, and runs git as `forkmesh-node`. Since 2026-10-07 it first keeps the
tip under the executor's trash (`refs/forkmesh/trash/branch/<b>/<token>`, or
`agent-branch/` for `agent/*`; token = the first 16 hex of
`sha256("<ref> <expect>")`, `delete_ref_trash`), so peer reconcile no longer
fetches it back and the source checkout learns of the delete from
[the listing above](#listing-the-branches-the-cores-deleted-2026-10-07);
`delete-refs` does the same per line and keeps a branch whose trash could not
be written. Run it on n1, n2 and n3 back to back all the same: a core that
still has the branch would hand it back to the others on their next fetch.

Merged `agent/` branches of runner sessions are removed from the phone or the
web instead (**Merge & clean up**, 2026-09-30): the merge executor's
`delete-agent-branch` runs on every core, saves the tip under
`refs/forkmesh/trash/agent-branch/` first, and peer reconcile no longer
fetches a trashed tip back ([server/merge-executor.md](server/merge-executor.md#agent-branches)).

For many branches at once, `next_fleet.py delete-refs --repo forkmesh/forkmesh
--from FILE --execute` takes `<branch> <object id>` lines, applies the same
checks to each, runs every core in parallel and repeats until no core deletes
anything, because a core that syncs mid-run fetches the branches back from a
core that still has them. `tools/prune_merged.py --remote` builds that list
from `origin` ([tools/prune-merged.md](tools/prune-merged.md)).

## Hooks

After an accepted push to a core that moved refs, `core_push.record_accepted_push`
raises the same events a node publication raises, naming the pusher:

- `repository.push` webhook, which also emits the `repository.push` ForkMesh
  Apps event (`queue_repo_webhook_event`). `data` carries `defaultBranch`,
  `before`, `after`, `refsDigest`, `defaultBranchMoved`, `pusher` and `refs`
  (every updated ref with `before`/`after`). Its event digest is the hash of
  the ref transitions, so a push to a side branch is its own event.
- `branch.push` webhook when the default branch moved.
- The code-push Ping (`notify_code_push`) to the owning account and the
  organization's readers, with `pusher` set; the pusher is not pinged about
  their own push.

Hosted repositories keep their own hooks (`hosted_git._pin_hosted_state`).

## Audit

Every accepted push (core and hosted) writes one `git.push` row per updated
ref in `sensitive_audit_log`: actor = the pushing account, target =
`<node>/<repo>`, details `ref`, `old`, `new`, `node` (the core that took it),
`credential` (`key`, `session`, or `docs-page` for the developer docs page)
and `commits` for a branch. The gateway
lists at most 40 ref updates per push (`X-ForkMesh-Ref-Updates`); a larger
push adds one row with `refsNotListed`. Authorization decisions stay under
`git.push_authorize`; tag moves under `git.tag_move_authorize` and
`git.tag_move`.

## Rollout

1. Gateway and daemon, one core at a time: `next_dns.py drain nN --execute`,
   `next_fleet.py install nN --steps daemon --execute`, check
   `next_ids.py status nN` and `/readyz`, then `next_dns.py undrain nN
   --execute`. The gateway must know `tagMoves` before the app sends one; an
   old gateway refuses a push that carries it.
2. App: `next_deploy.py all --execute` (applies `0205_git_tag_moves`).
3. Publisher (production repo): `python3 tools/source_publisher.py install`.

## Tests

- `app/tests/test_core_push_ref_policy.py`: real `git push` through the
  gateway: ref transitions and commit counts, tag moves with and without a
  grant, no deletes, no rewinds, grant parsing.
- `app/tests/test_git_push_route.py`: owners admitted and recorded, non-owners
  and anonymous refused before any core, tag moves in the signed target.
- `app/tests/test_core_push.py`: owner-only permission, tag-move
  authorization, audit rows, webhooks, Apps events and Pings.
- `server/core_push_test.go`: a moved tag is adopted only for the admitted
  state.
- Production repo `tools/test_source_publisher.py`: the audited tag move,
  stale branches, absorbing owner pushes.
