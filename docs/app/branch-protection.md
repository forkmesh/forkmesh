# Protected default branch and the Actions switch

Code: `app/src/branch_protection.py`, wired into the write paths listed
below. Tests: `app/tests/test_branch_protection.py`,
`app/tests/host/test_instance_repo_e2e.py`,
`app/tests/test_instance_actions.py`.

A repository can protect its default branch. The default branch then changes
only by merging a pull request that an organization owner or admin approved,
and that approver must not be the author. The rule exists for repositories
whose default branch deploys somewhere, for example an instance that builds
itself from its own repository.
Before this, ForkMesh had no protected branches: pull rules
([../server/pull-rules.md](../server/pull-rules.md)) check a pull request's
content, the node operator policy blocks only deletes and force pushes, and an
organization owner could push to `main`, edit files on it, merge branches into
it from the Branches page, or waive the review gate.

## Settings

Both switches are keys of the repository's `repo_feature_settings` row, next
to `issues`, `pulls` and `discussions` (`REPO_FEATURE_DEFAULTS` in
`entry.py`):

| Key | Default | Meaning |
| --- | --- | --- |
| `protectDefaultBranch` | `false` | the default branch, and every `forkmesh/*` branch, is protected |
| `actions` | `true` | `false`: no workflow of this repository runs, and no Actions grant is issued for it |

- **Web:** repository Settings → Features (two checkboxes). The save goes
  through `POST /api/repo/<owner>/<repo>/about` with `features`. Changing
  either key needs an organization owner or admin for an organization
  repository, or the owner for a personal one (`may_configure`), not just a
  repository admin, and is written to the sensitive audit log as
  `repository.protection_settings`. A settings-only save of a hosted
  repository no longer needs the owner to have a desktop key
  (`settings_only`): the `account_required` check exists because About text
  is delivered to the owner's desktop, which hosted repositories do not have.
- **Operator:** `python -m forkmesh_host instance-repo ...
  --protect-default-branch | --no-protect-default-branch` and
  `--actions on|off` ([../INSTANCES.md](../INSTANCES.md#importing-a-repository-on-the-server-instance_repopy)).
  The switches are applied right after the repository row exists and before
  the fill, so the import's own push hooks already see them.

The default branch is the catalog record's `branch` (what the server
published), never a value from the request. A repository whose settings or
default branch cannot be read is treated as protected: writes answer
`protection_unavailable` (503) instead of passing.

The `forkmesh/*` branches hold pull requests, reviews and other collaboration
records that the git node writes. Protecting them stops anyone from pushing a
review file that names an owner as the approver.

## Where it is enforced

| Write path | Check |
| --- | --- |
| `git push` over HTTPS, web file edits, docs edits (`_https_mirror_push_send`) | the app signs the protected refs into the receive-pack target (`?protect=refs/heads/main refs/heads/forkmesh/`); the gateway (`mirror_gateway.enforce_protected_refs`) refuses any command on them. The query is part of the signed capability, so a client cannot drop it. |
| `git push` over SSH (`ssh_gateway_routes`) | the authorize call does not see refs, so receive-pack is refused for the whole repository; push over HTTPS |
| Branches page create/update/delete/merge (`repository_branch_ops_handler`, `agent_branches.core_branch_op`) | refused when the moved branch, or a merge's base, is protected; a resumed core op is checked again because it carries its own base |
| Agent branch merge (`agent_branches._merge`) | refused when the merge base is protected |
| Pull request merge (`repository_pull_merge_handler`) | `merge_refusal` before the job is queued (below) |
| Actions (`runner_actions.lease`) | a job on a protected branch gets no `actions-push` grant; it still runs, with its token and secrets, but cannot push |
| Operator `instance-repo` | not checked: the root shell on the server is the authority, and it never counts as a merge |

## Approvals

The git node's review gate (`server/merge.go`) only compares signing keys:
any signed approval from a key other than the author's counts, so one person
with a desktop key and a browser key could approve their own pull request,
and anyone signed in can post an approval. Protection adds an account-based
check in the app:

- When a review event reaches `pulls_handler`, `_record_pull_review` stores
  `(repository, pull, account, state)` in `pull_review_ledger` (migration
  0274), where the account is the signed-in session that posted it. Reviews
  posted with an app token (`fmat_...`) or without a session are not
  recorded. The cookie session is already origin-checked for POSTs.
- `merge_refusal` reads the pull request record (`pull.md`) for its base and
  author. Merges into other branches pass. Into a protected branch:
  - the review waiver (`overrideReview`) is refused;
  - the latest review per account counts (`dismissed` clears it);
  - an approval counts only from an organization owner or admin (the owner,
    for a personal repository) who is neither the author's account (from the
    pull's signing key) nor its `authorName`;
  - a `changes_requested` from an owner or admin blocks until approved or
    dismissed.
- The merging account must still be an organization owner, and the node's
  own review gate, required checks and pull rules still apply.

An approval is not tied to the head commit: new commits pushed to the pull
request's branch after an approval keep it. The owner who merges sees the
final diff.

## The Actions switch

With `actions` off for a repository:

- `runner_actions.enqueue` queues nothing (push, merge, issue, schedule and
  dispatch events all go through it);
- `runner_actions.lease` cancels jobs queued before the switch, so no read or
  push grant is issued for them;
- the run controls answer `actions_disabled` for dispatch and re-run (cancel
  still works), and the repository's Actions tab is hidden.

Workflows already running when the switch is turned off finish; their push
grants expire with the job.
