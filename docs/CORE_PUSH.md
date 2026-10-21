# Owner pushes to the core nodes

The cores hold the flagship repository and every repository whose namespace is
a core node's account. Owners push with plain `git push` over HTTPS:

```
git push https://app.forkmesh.com/<owner>/<repo>.git <branch>
```

The per-core names serve the same app and take the same pushes.

Code: `app/src/entry.py` (`_https_mirror_push_proxy`, `_core_push_permitted`,
`_admit_core_push_state`, `_ref_updates_from`), `app/src/core_push.py` (tag
moves, branch deletes and trash listing, audit, hooks),
`tools/mirror_gateway.py` (`enforce_push_ref_policy`, `X-ForkMesh-Ref-Updates`),
`server/gateway.go` (`adoptAdmittedTagMoves`), migration `0205_git_tag_moves`.
Each accepted branch push is also recorded for the pusher's "recent pushes"
bar ([app/recent-push-banner.md](app/recent-push-banner.md)).

## Who can push

| Repository | May push |
|---|---|
| Linked to an organization (`org_repos`) | an organization **owner** |
| Personal (a core node's namespace without an organization) | the account that owns the node |

Anyone else gets `403 push_forbidden` before any core is contacted, audited
as `git.push_authorize`. Anonymous requests get a `401` Basic challenge.

Credentials:

- Basic with the account name or email as the username and either the
  session-bound push credential (`fmp1.…`, from
  `POST /api/hosted-repositories/push-credential`) or, for an account without
  two-factor authentication, the account password.
- Signed requests (`forkmesh-git-push-v1`, `X-ForkMesh-Pubkey/-Ts/-Sig`) with
  the account's primary or an enabled device key.

Admins may also commit one edited `docs/**/*.md` file through the developer
docs page, under the same ref rules. There are no SSH pushes and no personal
access tokens.

**Account password over git** (`hosted_git._password_git_account`). People
typed their sign-in email and password into `git push` and got
`Authentication failed`: only the account name and an `fmp1` credential were
accepted, and the credential had to be minted on the website first and
lasts seven days.
The password path checks exactly what the website login checks
(`_account_password_record`: active user account, PBKDF2 password) and spends
the same `login_attempts` budgets (`_login_attempt_keys`, `_login_record_fail`,
`_login_clear`), so git cannot be used to guess passwords faster than the
sign-in page. It refuses a password for an account with two-factor
authentication because Git cannot supply the second factor; sign in through
the website and use a session-bound push credential instead. Push
permission is re-checked the same way for both credentials, and the audit row's
`credential` says `password` or `session-mac`. An instance turns the password
path off with `GIT_PASSWORD_AUTH=0`. Private hosted clones accept the same
credentials. Tests: `test_account_name_or_email_and_password_push` and the
tests after it in `app/tests/test_hosted_repositories.py`.

**Retired cores.** A core removed from the fleet is listed in
`[hosted] retired_cores`; `deploy/core/render.py` writes
`FORKMESH_VAR_HOSTED_GIT_CORE_OWNERS` (live plus retired cores), and
`hosted_repos.core_owners` backs `_core_push_nodes`, so repositories owned by a
retired core's account stay core-held while pushes go only to live cores.

**"Open a pull request" after a push to a fork** (`_fork_push_notice`,
`git_http.with_sideband_message`). A contributor who pushed a branch to their
fork had to find the compare page themselves. After an accepted receive-pack
(`POST`, 200) whose `X-ForkMesh-Ref-Updates` name a new or moved branch other
than the default, on a repository whose catalog record has `forkedFrom` (or
a contributor's `pr/<account>/` branch on the repository itself, see
[HOSTED_REPOS.md](HOSTED_REPOS.md)), the proxy reads the result and adds one
side-band 2 packet before the final flush:

```
remote: Open a pull request for 'fix' by visiting:
remote:      https://app.forkmesh.com/<you>/<repo>/compare/main...fix
```

On a fork that page targets the parent (`pullRequestParent`); for a `pr/`
branch it opens a pull request on the repository itself. Results without side-band
(`docs_book.py`'s report-status push) and non-fork pushes are passed through
untouched, and the body is only read when the line applies. Up to five
branches are listed. `_https_mirror_push_send` is unchanged. Tests:
`app/tests/test_fork_push_notice.py`, including a stock `git push` against
`git http-backend` that prints the lines.

## Ref rules

| Ref | Rule |
|---|---|
| default branch, protected branches, `forkmesh/*` | fast-forward only, for everyone |
| other branches | force-push allowed for accounts that may write them ([force pushes](#force-pushes)) |
| any branch | never deleted by a push (use the Branches page) |
| new tag | allowed |
| existing tag | moves only to the exact object of a pending owner tag move; never deleted |
| outside `refs/heads/` and `refs/tags/` | refused |

## Force pushes

Updating a pull request after `git commit --amend`, `git rebase main` or a
squash needs a non-fast-forward update. It is allowed GitHub-style: on any
branch except the default branch, protected branches and the `forkmesh/`
ledger branches, by an account that may write that branch.

**Who may rewrite what.** The decision needs the pusher, so it is not a static
`receive.denyNonFastForwards`:

- `_https_mirror_push_send` adds `rewrite=<patterns>` to the router-signed
  receive-pack capability, only on the user push route
  (`_https_mirror_push_proxy`, which the SSH relay also reaches). A writer
  (owner, organization writer, signed key) gets `rewrite=refs/heads/`; a
  contributor gets `rewrite=refs/heads/pr/<account>/`, the same prefix as
  `pushPrefix`. Web edits (`repository_file_edits.py`, `docs_book.py`) never
  send it.
- The gateway (`tools/mirror_gateway.py`, `parse_rewrite_refs`,
  `rewritable_refs`) refuses a contributor grant naming refs outside its
  prefix, then narrows the grant to the refs this push updates that are
  under `refs/heads/`, not `HEAD`'s branch on that core, not under
  `refs/heads/forkmesh/` and not protected (`protect=`, which already refuses
  any update to those refs).
- With a grant, receive-pack runs with `receive.denyNonFastForwards=false` and
  `core.hooksPath` pointing at a gateway-owned directory (inside its private
  runtime directory) whose `pre-receive` (`push_rewrite_hook_script`) reads
  the `old new ref` lines and refuses any non-fast-forward branch update whose
  ref is not in `FORKMESH_PUSH_REWRITE`, the exact list the gateway passed in
  the environment. It then hands the same input to the operator-policy hook,
  when one is configured, so `blockForcePush` rules still apply.
- Fail closed: no `rewrite=` field (an older app, a web edit, a runner grant)
  or a missing hook file keeps `receive.denyNonFastForwards=true`; an empty
  `FORKMESH_PUSH_REWRITE` makes the hook refuse every rewrite.
  `receive.denyDeletes=true` always stays, and `enforce_branch_deletes`
  answers a branch delete with a pointer to the Branches page: a pushed delete
  would be fetched back from the other cores.

**Messages.** A forced push to the default branch prints "`<b>` is the default
branch: it only moves forward" and suggests `git pull --rebase origin <b>`. A
plain `git push` of a branch the server moved (after **Update branch** or a
web edit) is refused by git itself with "fetch first"; the docs and the
accepted-push notice recommend `git pull --rebase`, then
`git push --force-with-lease`. An accepted forced update adds `remote:` lines
(`_fork_push_notice`) naming the old and new tips and a compare link.

**Nothing is lost.** Before the push lock is released the gateway checks each
granted ref that moved (`merge-base --is-ancestor old new`); for a forced one it
stores the old tip at `refs/forkmesh/history/<branch>/<10-digit sequence>`
(fallback `refs/forkmesh/history-tips/<oid>`) and keeps the newest 20 per
branch (`PUSH_HISTORY_KEEP`). `refs/forkmesh/` is hidden from upload-pack and
receive-pack, so old commits stay resolvable for reviews and comments without
being advertised. To restore, an operator runs
`git update-ref refs/heads/<b> <oid>` on a core (or pushes the commit from a
clone that has it).

**Reporting.** The `X-ForkMesh-Ref-Updates` entry of a forced update carries
`forced: true` and `kept` (the history ref), and `record_accepted_push` audits
`git.force_push`. The open pull requests of the branch are reported for every
branch push, forced or not (see [pull requests follow their
branch](#pull-requests-follow-their-branch)). Older ledgers keep `force-push`
events from before that change; the timeline still renders them.

### Pull requests follow their branch

A pull request opened from a branch of the same repository (a writer's
`feature` or a contributor's `pr/<account>/<topic>`) follows that branch the
way GitHub does: a push to the branch updates the pull request.

**Why not re-sign the pull.** The author's signature (`verify_pull_event`)
covers title, base, head name, `changes.patch` and `commits.mbox`, never a
commit id. `headOid` and `headBaseOid` are the node's own record of the commit
the signed series replays to (`recordPullHead`), unsigned like `status`. So a
push leaves the signed creation diff alone and records the new head next to
it, which keeps every old signature valid.

**Detection.** For every accepted push that moves an existing branch (not the
default branch, not `forkmesh/`, not a new branch), the gateway lists the open
pull requests whose `pull.md` names that branch as `head`
(`open_pulls_for_head`, at most 8 branches per push and 10 pulls per branch)
and puts them in the update's `pulls`. `head` values of the form
`<copy>:<branch>` (patch-backed pulls from another copy) never match, so those
still update only by resubmitting.

**Who may move a pull.** Only a push the platform accepted (HTTPS gateway,
contributor push, core push): the gateway already checked that the pusher may
write that ref (owner, organization writer, or a contributor inside
`pr/<account>/`). `may_synchronize` checks again that a contributor push only
moves `pr/<pusher>/` branches, and an anonymous push moves nothing.

**The event.** `record_accepted_push` queues one `synchronize` event per pull
in `pull_inbox` (`type, id, author, authorName, ts, ref, before, after,
commits, forced, body`, `sig: ""` like force-push and ForkBot events), with
`source: "synchronize"`. The id is `synchronize-<n>-<after[:16]>`, so the same
(pull, head) is recorded once however often it is queued.

**Applying it (node).** `materializePullInbox` checks the event before it
writes anything (`pullSynchronizeTarget`): the pull is open, `head` is a plain
branch of this repository that equals the event's `ref`, `after` is a commit
the repository has and the branch still contains (an older event for a commit
a later force push dropped is skipped), and the pull is not already at
`after`. It then writes the event file and, for a patch-backed record, sets
`headOid: <after>`, `headBaseOid: <merge-base of after and the base branch>`
and `syncedHeadOid: <after>` in `pull.md` (`applyPullSynchronize`) and moves
`refs/pr/<n>/head`. `derive: branch` records already follow their branch, so
only the timeline event is written. `ensurePullHeadRefs` publishes `headOid`,
so the sweep keeps the synced head instead of reverting it.

**Readers.** `syncedHeadOid` equal to `headOid` means "the head is a real
branch commit, not a replay of the signed series":

- the web reads Files changed and Commits from `compare(headBaseOid, head)`
  (`loadRepoPullSyncedPatch`, `loadRepoPullCommits`); the host's first paint
  (`repo_views.plan_pull`) does the same;
- the merge trial, checks and merge box use `headOid` as before;
- the merge executor merges that commit instead of replaying the series
  (`syncedPullHead`; an unresolvable synced head fails with `stale_head`);
- anything else that reads `headOid` or `refs/pr/<n>/head` follows too.

**Re-queued work.** When the node acknowledges the inbox rows, the relay looks
at the acknowledged `synchronize` rows (`_pull_inbox_synchronized`) and calls
`core_push.pulls_synchronized`, which hands the moved branches to the pull
hooks a build has (`pull_attention_moved`, `ai_review_note_push`,
`pull_actions_sync_repo`, each optional). Waiting for the acknowledgement
matters: before it, `pull.md` still names the old head.

**Timeline.** "`<pusher>` pushed N commits to `<branch>` `<after7>`" or
"force-pushed `<branch>` from `<before7>` to `<after7>`", each with a compare
link; reviews submitted before the newest push show "on an older commit".
There is no "dismiss stale reviews" rule, so approvals still count.

**Repair.** Pulls whose branch moved before this existed (or while the node was
old) are moved with the host CLI, dry run first:

```sh
cd /opt/forkmesh-instance/src/app/host && /opt/forkmesh-instance/venv/bin/python \
  -m forkmesh_host pull-resync --owner <storage-owner> --name <repo> \
  [--number <n>] --dry-run --env-file /etc/forkmesh-instance/app.env \
  --db sqlite:/var/lib/forkmesh-instance/app.db \
  --data-dir /var/lib/forkmesh-instance/data
```

It reads the hosted repository as `forkmesh-node`, lists every open pull with
an action (`sync` or `skip` and why) and, without `--dry-run`, queues the same
`synchronize` events as a push (author `--actor`, default `--owner`), then
asks the local node to drain its inboxes now (`POST /v1/control/sync` on the
loopback `listen` address from `--node-config`). That wake is needed because
`notify_repo_host` reaches the node through the Durable Object host of the
process that calls it: on a self-hosted instance that host lives inside the
serving app, so a doorbell rung from a CLI process reaches no socket and the
row would wait for the node's next catch-up (a reconnect or a push). A pull is
moved only when its head branch exists, contains the pull's base, and holds a
commit whose subject matches one in the signed `commits.mbox`; `--number`
names a pull explicitly and skips the subject match. The node applies the
events with the checks above, so a dry run is safe and a real run is
idempotent.

**Replicas.** Cores converge by fetching (`reconcileUpstreamRefs`), which never
rewinds a branch and lets the newer commit win a divergence. So the app
records each forced tip in the hosted repository record (`forced`, newest 32,
kept 7 days, `hosted_repos.note_forced`) before it moves `pushed_at`, and the
replica desired set carries `forced: {ref: oid}`. A replica that fetches
exactly that tip for that ref keeps its own old tip under
`refs/forkmesh/history/` and moves the branch (`adoptForcedBranches`, never
`HEAD`'s branch or `forkmesh/`). Every core also refuses to move a branch to a
tip recorded in its history (`supersededBranchTips`), so a stale peer cannot
bring a replaced commit back.

**SSH gateway.** The relay mode forwards to the HTTPS route and inherits all of
this. The local mode (`ssh_gateway.py` without `httpGit`) sets no deny rules
and relies on the operator-policy hook, as before.

### Moving a tag

1. `POST /api/git/tag-moves` `{"owner", "repo", "ref", "target"}`, by session
   or signed `forkmesh-tag-move-v1\n{owner}\n{repo}\n{ref}\n{target}\n{ts}`.
   Only core-held repositories and permitted pushers; the row lives 15
   minutes in `git_tag_moves`.
2. `git push --force origin refs/tags/<tag>` within 15 minutes; the app puts
   the pending moves into the router-signed capability and the gateway allows
   exactly those.
3. The move is audited as `git.tag_move`.

## Consistency

1. The app streams the push to one core.
2. `_admit_core_push_state` records that core's freshly signed routing digest
   and sends the `commits` wake to every core, including the canonical owner
   when it is a core. The wake goes out even when the digest could not be
   admitted (then without state, as a plain sync request).
3. Other cores fetch from the pushed core and converge fast-forward only
   (a recorded force push excepted, see [force pushes](#force-pushes)); a
   moved tag is adopted only when it yields exactly the admitted digest.
4. The source publisher pushes the source checkout the same way and absorbs
   commits owners pushed directly (fast-forward, or a clean merge; conflicts
   are logged and left alone).

Robustness: before every fetch the daemon drops broken or dangling loose refs
and stale lock files (`dropBrokenRefs`); failed fetches retry while avoiding
the failing peer; a node serving broken refs fails its integrity check and is
routed around.

## Deleting branches

A push never deletes (the gateway answers with a pointer to the Branches page). Branch deletion goes through the merge executor, which
first saves the tip under `refs/forkmesh/trash/branch/<b>/<token>` (or
`agent-branch/`) so peers do not fetch it back.

- `POST /api/git/branch-deletes` `{"owner", "repo", "branch", "expect"}`
  (signed `forkmesh-branch-delete-v1\n…` or session; same owners as tag
  moves). Refuses `main`, `master`, `HEAD` and `forkmesh/*`; deletes only if
  the branch is still at `expect`, then fans out to every core. The source
  publisher uses it for branches deleted in the source checkout.
- `POST /api/git/branch-trash` `{"owner", "repo"}` (signed
  `forkmesh-branch-trash-v1\n{owner}\n{repo}\n{ts}`) returns the union of the
  cores' trash entries `{branch, oid}`, with unanswered cores named. The
  gateway serves its list only behind a router capability; deleted tips are
  never exposed to ordinary readers. Results are cached 30 s per repository.
  The desktop uses it to delete local branches the cores deleted.
- A delete still running answers 202 with `requestId`, `node` and `pinBase`;
  the caller re-posts them until it settles, and a re-post can answer 503
  `merge_status_pending` or `merge_node_unavailable` (`retryable: true`).
  Those 503s, and the `cores_unavailable` 503 from both routes, carry
  `x-forkmesh-expected-degraded`, so they are not logged as API errors: a
  publisher poll turned red the API row on /status on 2026-10-18. A core
  that is really down shows on its own /status row.
- Operator: `python3 tools/next_fleet.py delete-ref nN --repo <owner>/<repo> --ref refs/heads/<b> --expect <sha> --execute`
  (only branches merged into `main`, trash first, run on every core) and
  `delete-refs --from FILE` for many at once.

## Hooks

An accepted core push that moved refs raises the same events a node
publication raises: the `repository.push` webhook and Apps event (with
`pusher` and every ref transition), `branch.push` when the default branch
moved, and the code-push Ping to the owner and organization readers.

## Audit

Every accepted push writes one `git.push` row per updated ref in
`sensitive_audit_log` (actor, target, ref, old, new, node, credential kind,
commit count). The gateway lists at most 40 ref updates per push; larger
pushes add a `refsNotListed` row.

## Rollout and tests

Roll the gateway and daemon one core at a time (drained), then the app. Tests:
`app/tests/test_core_push_ref_policy.py`, `test_git_push_route.py`,
`test_core_push.py`, `test_node_event_push.py`, `server/core_push_test.go`.
