# Pull request merge box

Part of the [App](README.md). The conversation tab of an open pull request
ends in one merge box (`renderRepoPullMergeBox` in
`app/public/dashboard/js/06-repo-content.js`, styles `.fm-merge-*` in
`app/public/dashboard/shell.html`). One box replaced three panels
(readiness, merge control, update button) that each told part of the story.

## What it shows

`repoPullMergeModel` turns what the page already reads into one row per thing
that can block a merge, each with the action that clears it:

| Row | Source | Action |
| --- | --- | --- |
| Review | signed reviews on the pull ledger (`pullReviewPolicy`) | Copy review link (to the Files changed tab) |
| Checks | the mirror's Actions runs for the current head, and `.forkmesh/required-checks.json` read at the reviewed base commit | Re-run failed (`actions/control` `rerun`), Run checks / Open Actions |
| Branch | the mirror's merge trial (`merge-preview`) | Update branch, or how to resolve conflicts |
| Rules | `pulls/<n>/rules` | none: blocking rules cannot be waived |

When the checks come from the pull's own runner (`pulls/<n>/checks`, source
`pull`), the Checks row lists each run with state, duration and log link
(`renderRepoPullCheckList`). Checks waiting for a maintainer's approval are
named first (`repoPullCheckApprovalText`); the Approve and Always run buttons
stay in the pull's Checks section (`renderRepoPullCheckApproval`).

The footer says whether the pull can merge, why not, and holds the controls.
Only someone who may merge (the repository's owner, or for an organization's
repository an organization owner) sees the controls; everyone else sees the
same rows and who can merge. The header status button
(`repoPullMergeStatus`) shows them "Waiting for a maintainer to merge"
instead of "Ready to merge", because contributors read a green "Ready" as
permission to merge their own pull.

*Re-run failed* is shown only to someone who may merge and only when the
Actions read named its executor node: `actions/control` refuses a request
without `requestId`, `action` and `node` (as the Actions tab sends it,
`sendRepoActionsControl`) and answers 403 to anyone but the owner.

The conflict and "behind" rows print git commands containing the branch
names the pull's author chose. Git allows `;`, `$`, `|` and backticks in
branch names, so each name not matching `[A-Za-z0-9._/@+=:,-]` is
shell-quoted (`shellWord`).

- **Independent review.** The executor requires one signed approval from
  someone other than the author and no outstanding request for changes.
  Without it *Merge pull request* stays disabled and the organization owner
  gets a separate *Merge without review* button, which confirms and then
  sends `overrideReview: true`; the relay audits it and the executor records
  it in the ledger. The flag is sent only when the gate is unmet, so an
  approved pull never carries a waiver.
- **Merge method.** Mirror merges create a merge commit (`git merge-tree`,
  [merge-executor.md](../server/merge-executor.md)). Squash and rebase are
  listed but disabled until the executor supports them.
- **Behind the base.** A warning, not a blocker: a branch-backed pull lands
  as a merge commit, and a patch-backed pull is replayed onto the newest base.

## Merged and closed pulls

A merged pull shows one row instead of the merge rows: "Pull request
successfully merged and closed", saying the head branch is in the base
(`renderRepoPullDoneBox`). A closed pull shows "Closed with unmerged
commits". The old Review, Checks and Branch rows under a disabled Merged
button read as if the pull could still merge. The live progress of a merge
made on this page still shows inside the merged row.

The review form keeps Comment but drops Approve and Request changes once the
pull is not open (`renderPullReviewForm`).

The conversation gets a "merged" entry (`repoPullMergedEvent` in
`buildRepoPullTimeline`) from `pull.md`'s `status: merged`, `mergedAt` and
`mergedBy`, placed by time, or last when `mergedAt` is missing. It names the
merger only when `mergedBy` is a key the page can name (the author's); a mesh
merge (`server/merge.go`) records no `mergedBy`, so the entry reads "Merged
`head` into `base`".

The desktop (`desktop/src/MainWindowPulls.cpp`) and the iPhone app add the
same entry; on the desktop it is a card in `renderPullThread`, the card above
the merge button (`m_pullMergeBlockers`) carries the merged or closed
sentence, and Approve and Request changes are hidden while the pull is not
open.

## Review chip in the pull request list

Each open row of the pull request list shows its review state instead of its
branches: **Approved**, **Changes requested** or **Review required**, with a
title such as "Approved by 2 reviewers". `review_state` in
`app/host/forkmesh_host/repo_views.py` (server-rendered list) and
`pullListReviewState` in `06-repo-content.js` (client) compute it the same
way, over each reviewer's latest review with the author left out
(`review_policy` / `pullReviewPolicy`): any request for changes wins, one
approval and no blockers is Approved, and a closed or draft pull with neither
shows no chip. The mirror gateway's `.forkmesh/pulls` listing
(`tools/mirror_gateway.py`) carries every committed review's
`(reviewer, state)` in file order so the list can compute it.

The chip is a summary, not the merge gate. On a protected branch only an
owner's or admin's approval counts
([branch-protection.md](branch-protection.md#approvals)), so a pull approved
by another contributor shows **Approved** while the merge box still waits.
Tests: `app/tests/test_pull_list_review_state.py`.

On the pull page a reviewer is an account, not a key (`pullReviewerKey`):
someone who reviews from a laptop and a phone gets one row and one vote, as
the relay's `merge_refusal` counts them. Only a verified `authorName` (bound
by the relay to the signed-in account) groups keys; an unverified claim
("name (unverified)") stays keyed by its signing key, so two strangers
claiming one name stay two reviewers. The author is left out by key or by
verified account name, so their second device cannot approve their own pull.
The list chip has only keys and groups by key. Tests:
`app/tests/test_pull_review_frontend.py`.

## Merge status button and compact header

An open pull shows a merge status button beside the copy-link button in its
header (`renderRepoPullMergeStatus`, styles `.fm-merge-status`). It reads the
same `repoPullMergeModel` (`repoPullMergeStatus`) and is re-rendered by
`updateRepoPullMergePanel` with the box:

- red when any row blocks (changes requested, failing checks, conflicts,
  unmet rules) or the footer is an error;
- orange when the merge waits (review required, checks running or
  incomplete, mergeability unknown) or a merge is in progress;
- green otherwise. Being behind the base does not change the colour.

The label names the first problem, a `+N` counter counts the rest, and the
tooltip lists every row title. Clicking it opens Conversation and scrolls to
the box (`showRepoPullMergeBox`). Merged and closed pulls show no button.

While a pull is open, the repository title row (name, Public, Fork, Star) is
hidden by a `:has()` rule in `shell.html`: the pull's title is the page
heading. The new-pull composer and batch view keep the row.

The Reviewers sidebar is compact, as on GitHub: one line for the review gate
(shield icon, approval and blocking counts), then one line per reviewer with
a state icon (check approved, file-diff changes requested, speech bubble
commented) and a history icon when the review is on an older commit. The
words live in each icon's tooltip and label.

## Update branch

A branch-backed pull (`derive: branch`, its head is a branch of this
repository) that is behind gets **Update branch**. It posts
`forkmesh.pull-update-v1` to `/api/repo/<o>/<r>/pulls/<n>/update`
(`repository_pull_merge_handler`, mutation `update`); the mirror merges the
base into the head branch (`update-pull-branch`, `_update_execute_locked`,
`server/merge.go`). The executor has no rebase mode, so none is offered.

- **Who.** Anyone who may push to the head branch
  (`_account_may_write_branch`): the repository's owner, an organization
  member with write permission or better, or a contributor whose head is in
  their own `pr/<account>/` namespace on a repository open to contributor
  pushes. Being the author is not enough, because a pull can name any branch
  as its head. The page offers the button on the same basis
  (`loadRepoPullUpdateAuthorization`); the relay decides.
- **Protected heads.** An update that would move a protected branch is
  refused (`branch_protected`), for owners too: a protected branch changes
  only by merging an approved pull.
- **Outcome.** A clean update shows "Synced this branch with `<base>`" and
  the time. On conflict nothing changes and the box says to resolve locally
  and push; there is no web conflict editor. The button is shown on a
  conflicting branch too, so pressing it gives that answer.
- **Patch-backed pulls.** A pull from someone else's copy (a fork, a web or
  cross-node submission) travels in the ledger as
  `commits.mbox`/`changes.patch`; there is no branch here to update. The
  mirror replays it onto the newest base at merge, so the row says it merges
  cleanly and needs no update rather than "N commits behind".
- A visitor sees the commands to merge the base locally and push.

## Live progress

After *Merge pull request* or *Update branch* the box shows a spinner, the
current step ("Verifying the reviewed commits…", "Merging into main…",
"Publishing…"), time since the click, a step list with a tick and the
mirror-reported duration for each finished step, and the mirror's name. A
merge ends with "Merged as `abc1234`" linking to the merge commit (an update
with "New head"). A failure marks the step that refused, keeps the reason and
*Merge again*, which resumes the same request id when the answer was "not
yet". A failure no mirror ever held keeps the plain line. Phases come from
the executor ([merge-executor.md, "Progress"](../server/merge-executor.md#progress)).

Before a mirror holds the job the step is "Waiting for an online mirror…";
after 8 seconds it becomes "Waiting for an online mirror to pick this up (no
mirror has accepted it yet)" and explains that the page keeps retrying, a
retry never applies twice, and a single-server node may be restarting. When
the mirror that took the job stops answering, the page names it and says only
that mirror may finish it.

Updates come from repeating the same request id (`postRepoPullMutation`), not
a new endpoint or socket: the repository dashboard has no realtime channel
for pulls, and the job lives on one mirror the relay proxies to. In flight,
the page polls after 1, 2, then every 3 seconds, restarting at 1 second on
each new phase, and stops on a terminal answer. While the tab is hidden it
polls every 10 seconds, outside the 150-second budget: the relay learns a job
finished only from these polls, so a page that stopped while hidden could
leave a finished merge unrecorded. Progress is painted only into the box of
the pull that asked (`pullDetailIsCurrent`), and a re-run job starts its step
list afresh. A poll changes only the step line's text, so a screen reader
announces each step once (`role="status"`, `aria-live="polite"` on that line;
the clock is `aria-hidden`). A one-second ticker updates the clock and stops
when no clock is on the page. With `prefers-reduced-motion` the spinner is a
still ring with a dot.

## "The pull-request metadata changed"

Every merge and update pins three object ids: the base tip, the head, and the
tip of the `forkmesh/pulls` ledger branch (`expectedPullsOid`). The mirror
refuses with `stale_pull_metadata` when the ledger tip moved, and it moves
for *any* pull activity in the repository (a comment, a review, another
pull's intake). Two causes made this stick: the pull page's read bundle
(`app/host/forkmesh_host/repo_views.py`, `/page-reads`) is served
stale-while-revalidate for up to `BUNDLE_STALE_SECONDS` (five minutes), so
the page pinned an old tip; and a retry resent the same request id, for
which the relay stores one terminal answer (`repo_merge_jobs`).

So:

- Before sending, the page re-reads the ledger tip, the pull record, its
  reviews and the merge trial with `fresh` reads that skip the page bundle,
  the early reads and the session cache (`refreshRepoPullMergePins`,
  `fetchRepoJson(..., { fresh: true })`), and pins what it just read.
- On `stale_pull_metadata` or `stale_base` it re-reads once more and retries
  once with a new request id.
- It compares what decides the merge before and after
  (`pullMergeFingerprint`: status, head, patch series, conflicts, requests
  for changes). If any changed, nothing is sent and the box lists what
  changed; a comment, another pull's activity or a moved base that still
  merges cleanly is no reason to stop.
- A request id is reused only for a non-final answer (relay unreachable,
  mirror still working, result pending), so a retry never merges twice.
  After a final answer the next attempt is a new request.

The same applies to Update branch.

## Repository tab strip on phones

The tab strip (Code, Issues, Pull requests, ...) scrolls sideways on a phone
and must not jump:

- `renderRepoDetail` carries the strip's `scrollLeft` across a re-render
  (for example when permissions arrive and add Agents or Settings),
  including from the server-rendered first view.
- Each label reserves its bold width with a hidden bold copy
  (`[data-repo-tab-label]::after`), so selecting a tab does not widen it.
- Selecting an off-screen tab scrolls just far enough to show it
  (`keepRepoActiveTabVisible`), only when the selection changed.
- The Discussions counter keeps invisible space while a count waits for a
  live mirror; counters use tabular figures and are rewritten only when the
  number changes.
- The strip contains its overscroll, hides its scrollbar, and gives icons a
  fixed box before the icon script replaces them.
- The server-rendered strip (`_tabs` in `app/host/forkmesh_host/repo_ssr.py`)
  draws the same tabs (including Security), counter pills (`...` until a
  count is known) and label markup as the dashboard, so the swap moves
  nothing.
