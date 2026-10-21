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
  `POST /api/hosted-repositories/push-credential`) or the account password.
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
sign-in page. It does not ask for the two-factor code, because git cannot
send one: the maintainer chose (2026-10-21) to let every account, admins and
two-factor accounts included, push with its password, so two-factor sign-in
does not protect git pushes. Push
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

## Ref rules

| Ref | Rule |
|---|---|
| any branch, including `main` | fast-forward only, never deleted by a push, for everyone |
| new tag | allowed |
| existing tag | moves only to the exact object of a pending owner tag move; never deleted |
| outside `refs/heads/` and `refs/tags/` | refused |

Owners cannot force or delete branches because cores converge by fetching
from each other fast-forward only: a rule the fleet cannot replicate would
leave cores disagreeing.

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
3. Other cores fetch from the pushed core and converge fast-forward only; a
   moved tag is adopted only when it yields exactly the admitted digest.
4. The source publisher pushes the source checkout the same way and absorbs
   commits owners pushed directly (fast-forward, or a clean merge; conflicts
   are logged and left alone).

Robustness: before every fetch the daemon drops broken or dangling loose refs
and stale lock files (`dropBrokenRefs`); failed fetches retry while avoiding
the failing peer; a node serving broken refs fails its integrity check and is
routed around.

## Deleting branches

A push never deletes. Branch deletion goes through the merge executor, which
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
