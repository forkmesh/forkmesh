# Stale pull requests

A repository can close pull requests that wait on their author. When a
maintainer comments or requests changes and the author does not answer,
ForkBot reminds the author after 1, 3 and 7 days, and closes the pull request
after 8.

Code: `app/src/stale_pulls.py` (relay), the `stale-pulls` cron phase in
`app/src/entry.py`, the two switches in the Features group of the repository
Settings tab (`08-repo-detail-network.js`, `REPO_FEATURE_SWITCHES` in
`06-repo-content.js`), migration `0298_stale_pulls`. Tests:
`app/tests/test_stale_pulls.py`.

## Turning it on

Off by default for every repository. A repository administrator (the same
people who can switch Issues or Pull requests off,
[repo-settings.md](repo-settings.md)) opens **Settings** on the
repository and ticks, in the Features group:

- **Close stale pull requests** (`stalePulls`).
- **When ForkBot closes a stale pull request, also delete its head branch**
  (`stalePullsDeleteBranch`, also off by default; it does nothing unless the
  first switch is on).

Both are stored with the other feature switches in `repo_feature_settings`
(`REPO_FEATURE_DEFAULTS`, [repo-settings.md](repo-settings.md)) and travel in
`POST /api/repo/<owner>/<repo>/about` `features`. There is no instance-wide
default: an instance that wants it on turns it on per repository.

Saving the switch on registers the repository in `stale_pull_repos` with the
time it was turned on; saving it off removes the registration and every
tracked pull. Feedback older than the switch is counted from the moment the
switch was turned on, so turning it on never closes anything sooner than 8
days later.

## When a pull request is waiting on its author

The relay reads the pull request's conversation (the `NNNN-<type>.md` event
files under `.forkmesh/pulls/<n>/`, at the ledger commit or the pull's own
ref; at most the newest 60 events) and walks it in time order:

| Event | Effect |
| --- | --- |
| A maintainer's comment, line comment, thread comment or reply, or a review that requests changes or comments | starts the wait from that event; a newer one restarts it |
| A maintainer's approving review | ends the wait |
| The author's comment, review, line comment, reply, or a resolved thread or suggestion | ends the wait |
| A push to the pull request (`synchronize`) or a new revision of a patch | ends the wait, whoever pushed |
| Comments by anyone else, and ForkBot's own comments | no effect |

A maintainer is anyone with write access to the base repository: the node
account that owns it, the person who owns that node
(`_account_owns_node`), or an organization member whose role or team gives
`write`, `maintain` or `admin` on a linked repository (`_org_write_allowed`).
These are the same people who can push to the repository. Accounts are
resolved from the event's signing key when it is bound to an account, else
from its recorded author name.

The relay also remembers the head commit when the wait starts. If the head
branch moves while the pull request waits (a push the conversation does not
record), the wait ends too.

Never tracked: drafts (`status: draft` or `draft: true`), pull requests that
are closed, merged or deleted, and pull requests whose author is a maintainer.

## Schedule

From the event that started the wait:

| After | ForkBot comment | Ping to the author |
| --- | --- | --- |
| 24 hours | "@bob, carol left feedback on this pull request ... it closes on <date>." | Pull request #7 is waiting on your reply |
| 3 days | Reminder, with the closing date | Pull request #7 is still waiting on your reply |
| 7 days | "Final reminder: this pull request closes in 1 day" | Pull request #7 closes in 1 day |
| 8 days | Why it closed and how to carry on, then the pull request is closed | Pull request #7 was closed after 8 days without a reply |

The timings are `STAGE_DELAYS` in `stale_pulls.py`. Each step is sent once.
If the cron could not run for a while, only the most recent step that is due
is sent (no burst of three comments), and the close always comes at least a
day after the final reminder, so the author is never closed without that
day's notice.

The comments are ordinary pull request comments by ForkBot (`author:
forkbot`, `authorName: ForkBot`, unsigned), queued in `pull_inbox` exactly
like the AI review's comments, and shown with ForkBot's avatar. The close is a
`status` event with `state: closed` from ForkBot, the same event a maintainer's
Close sends, so the mirror intake (`applyPullLifecycleEvent`) closes the
record and keeps its patch. Event ids are fixed per pull, wait and step
(`stale-pull-<n>-<wait start>-<step>`), so a repeated delivery is dropped by
the intake.

The ping is a `pull_attention` notification ("Reviews and conflicts on your
pull requests"), with `meta.reason: "stale"` and the step, deduplicated per
pull, wait and step.

Pull requests cannot be reopened, so the closing comment tells the author to
push the update and open a new pull request that mentions the old number, or
ask a maintainer to pick it up.

## Deleting the head branch

Only with `stalePullsDeleteBranch` on, only at the close, and only when all of
these hold; otherwise the branch is kept and the closing comment does not
mention it:

- the pull request is branch-backed (`derive: branch`) and its head is a
  branch of this repository (no `owner:branch` head, never a patch from
  someone else's copy, never a `forkmesh/` branch);
- the repository is served by its owner's node (a repository the cores host,
  `source: hosted-web`, is skipped: its deletion is a request-bound job);
- the branch is not the default branch and not protected
  (`_branch_protection.push_protection`);
- no delete of that branch is already queued and the node's queue has room.

The delete is queued in `branch_op_inbox` as `{"op": "delete", "branch",
"expectedOid": <head commit seen at the close>, "requestedBy": "forkbot",
"source": "stale-pull"}`, the same operation as the Branches tab's Delete
([branch-ops.md](branch-ops.md)): the node deletes it with an exact-tip
`update-ref -d`, so a branch that moved after the close is kept.

## Cron

The `stale-pulls` phase runs every five minutes (minute 1, 6, 11, ...). Each
run takes the two registered repositories that are due and, for each, reads
the pull list (`tree` of `.forkmesh/pulls` at `forkmesh/pulls`), the records
of at most 30 pull requests (one `blobs` read, plus at most four pull
requests on their own refs), and the conversation of at most three open pull
requests. A repository with more pull requests is worked through over several
runs with a cursor, then waits an hour before the next sweep. So a reminder
can arrive up to a little over an hour after it is due on a small repository,
more on a large one. Every read is charged to the phase's subrequest budget
and the run stops when it is spent (`_cron_budget_exhausted`).

The database side is bounded too: the due repositories come from the
`next_at` index (`LIMIT 2`); the closed and draft pull requests of a batch
drop their `stale_pulls` rows in one `DELETE ... number IN (...)` instead of
one statement each, because a sweep passes every closed pull request of the
repository again each hour and on the cores each write is a Raft entry; and
the check for an already queued branch delete reads at most
`MAX_PENDING_BRANCH_OPS` rows of `branch_op_inbox`.

## Never twice

Each tracked pull request has one `stale_pulls` row with the step already
sent and a `version`. A step is claimed with a compare-and-set on the version
before anything is posted, so two cores, a restart or a repeated run cannot
post the same step twice. If the comment cannot be queued (inbox full, ForkBot
over its quota) the claim is undone and the next sweep tries again. A run that
dies between the claim and the post loses that step rather than repeating it.

## Data

```
stale_pull_repos (repo_bi PK, next_at, updated_at, data)
stale_pulls      (repo_bi, number, anchor_at, stage, stage_at, version,
                  updated_at, data)  PK (repo_bi, number)
```

`repo_bi` is the blind index of `<node>/<repo>`, the same key as the pull
inbox. `stale_pull_repos.data` is `encrypt_row` of the owner, node, public
owner, repository, the time the switch was turned on, and the sweep cursor.
`stale_pulls.data` is `encrypt_row` of the event id that started the wait, the
maintainer's name, the head commit seen, and whether the head moved. Rows
untouched for 60 days are pruned hourly. Repository deletion removes both
tables' rows (`_REPO_SCOPED_TABLES`); a `DATA_KEY` rotation re-keys both
(`tools/next_rekey.py`, [rekey](../operations/rekey.md)).

## Limits

- The relay reads records through the same repository read path as the
  conflict pings, so a repository whose pull list it cannot read (a private
  one, or a node that is offline) is skipped and retried an hour later.
- On a repository whose node merges its own inbox, the close takes effect
  when that node next drains the inbox, like any other queued close.
- A rename of the repository or its owner starts the registration over:
  save the switch again.
