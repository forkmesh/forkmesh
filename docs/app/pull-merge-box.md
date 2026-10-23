# Pull request merge box

The conversation tab of an open pull request ends in one merge box
(`renderRepoPullMergeBox` in `app/public/dashboard/js/06-repo-content.js`,
styles `.fm-merge-*` in `app/public/dashboard/shell.html`). It replaced three
separate panels (readiness, merge control, update button) that each said part
of the story.

## What it shows

`repoPullMergeModel` turns what the page already reads into rows, one per
thing that can block a merge, each with the action that clears it:

| Row | Source | Action |
| --- | --- | --- |
| Review | signed reviews on the pull ledger (`pullReviewPolicy`) | Copy review link (to the Files changed tab) |
| Checks | the mirror's Actions runs for the current head, and `.forkmesh/required-checks.json` read at the reviewed base commit | Re-run failed (`actions/control` `rerun`), Run checks / Open Actions |
| Branch | the mirror's merge trial (`merge-preview`) | Update branch, or how to resolve conflicts |
| Rules | `pulls/<n>/rules` | none: blocking rules cannot be waived |

When the checks come from the pull's own runner (`pulls/<n>/checks`, source
`pull`), the Checks row also lists each run with its state, duration and log
link (`renderRepoPullCheckList`). When those checks are waiting for a
maintainer's approval, the row says so first (`repoPullCheckApprovalText`).
The Approve and Always run buttons stay in the pull's Checks section
(`renderRepoPullCheckApproval`).

The footer says whether the pull can merge, why not, and holds the controls.
Only someone who may merge (the repository's owner, or for an organization's
repository an owner of the organization) sees the merge controls; everyone
else sees the same rows and who can merge.

*Re-run failed* is shown only to someone who may merge and only when the
Actions read named its executor node: `actions/control` refuses a request
without `requestId`, `action` and `node`, exactly as the Actions tab sends it
(`sendRepoActionsControl`), and answers 403 to anyone but the owner.

The conflict and "behind" rows print git commands with the pull's branch
names in them, which the pull's author chose. Git allows `;`, `$`, `|` and
backticks in branch names, so each name is shell-quoted (`shellWord`) unless
it is plain `[A-Za-z0-9._/@+=:,-]`, and a pasted command can only name the
branch.

- **Independent review.** The executor requires one signed approval from
  someone other than the author and no outstanding request for changes. When
  that is missing, *Merge pull request* stays disabled and the organization
  owner gets a
  separate *Merge without review* button. It asks for confirmation and only
  then sends `overrideReview: true`, which the relay audits and the executor
  records in the ledger. The flag is sent only when the gate is actually
  unmet, so an approved pull never carries a waiver.
- **Merge method.** Mirror merges create a merge commit
  (`git merge-tree`, `docs/server/merge-executor.md`). Squash and rebase are
  listed but disabled until the executor supports them.
- **Behind the base.** Being behind does not block a merge that is clean: a
  branch-backed pull lands as a merge commit, and a patch-backed pull is
  replayed onto the newest base. It is shown as a warning, not a blocker.

## Merge status button and compact header

An open pull request shows a merge status button beside the copy-link button
in its header (`renderRepoPullMergeStatus`, styles `.fm-merge-status`), so a
reviewer sees whether it can merge without scrolling to the box. It reads the
same `repoPullMergeModel` (`repoPullMergeStatus`) and is re-rendered by
`updateRepoPullMergePanel` whenever the box is:

- red when any row blocks the merge (changes requested, failing checks,
  conflicts, unmet rules) or the footer is an error;
- orange when the merge is waiting on something (review required, checks
  running or incomplete, mergeability unknown), or a merge is in progress;
- green when nothing is in the way. Being behind the base does not change the
  colour, since it does not block a clean merge.

The label names the first problem, a `+N` counter counts the others, and the
tooltip lists every row title. Clicking it opens the Conversation section and
scrolls to the merge box (`showRepoPullMergeBox`). Merged and closed pulls show
no button: the state pill already says it.

While a pull request is open the repository's own title row (name, Public,
Fork, Star) is hidden by a `:has()` rule in `shell.html`, because the pull's
title is the page heading and those repository actions stay a click away on
the Code tab. The new-pull composer and batch view keep the row.

The Reviewers sidebar is compact, the way GitHub draws it: one line for the
review gate (shield icon, approval and blocking counts as icons), then one
line per reviewer with a state icon (check approved, file-diff changes
requested, speech bubble commented) and a history icon when the review is on
an older commit. The words live in each icon's tooltip and label.

## Update branch

A branch-backed pull (`derive: branch`, its head is a branch of this
repository) that is behind and merges cleanly gets **Update branch** for an
organization owner. It posts `forkmesh.pull-update-v1` to `/api/repo/<o>/<r>/pulls/<n>/update`
(`repository_pull_merge_handler`, mutation `update`); the mirror merges the
base into the head branch (`update-pull-branch`, `_update_execute_locked` and
`server/merge.go`). Only merging is supported: the executor has no rebase
mode, so none is offered.

- A pull from someone else's copy (a fork on another node, a web or
  cross-node submission) is patch-backed: its commits travel in the ledger as
  `commits.mbox`/`changes.patch`, there is no branch here to update, and no
  maintainer can push to the author's copy. The box says so, says that only
  the author can refresh it by rebasing and resubmitting, and that it can
  still be merged while behind.
- A visitor or the author of a branch-backed pull sees the commands to merge
  the base locally and push.

## Live progress

After *Merge pull request* or *Update branch* the box shows what the mirror is
doing instead of one static line: a spinner, the current step ("Verifying the
reviewed commits…", "Merging into main…", "Publishing…"), the time since the
click, a step list with a tick and the mirror-reported duration for each
finished step, and the mirror's name. A merge ends with "Merged as `abc1234`"
linking to the merge commit (an update with "New head"); a failure marks the
step that refused (from the error when the last poll had not seen it yet),
keeps the reason and *Merge again*, which resumes the same request id when the
answer was "not yet". A failure no mirror ever held keeps the plain line.
Phases come from the executor ([merge-executor.md, "Progress"](../server/merge-executor.md#progress)).

Before a mirror holds the job the step is "Waiting for an online mirror…";
after 8 seconds it becomes "Waiting for an online mirror to pick this up (no
mirror has accepted it yet)" with what to expect: it keeps retrying while the
page is open, a retry can never apply it twice, and on a single-server
instance the node may be restarting. When the mirror that took the job stops
answering, the page names it and says only that mirror may finish it.

Updates come from the request the page was already repeating with the same
request id (`postRepoPullMutation`), not a new endpoint or socket: the
repository dashboard has no realtime channel for pull requests (the only
WebSocket is the notes editor's), and the job lives on one mirror the relay
proxies to. While a job is in flight the page asks after 1, 2, then every 3
seconds, starting again at 1 second on each new phase; it stops on a terminal
answer, waits while the tab is hidden (hidden time does not count against the
150-second budget), and asks nothing when no merge or update is in flight. A
poll changes only the step line's text, so a screen reader announces each new
step once (`role="status"`, `aria-live="polite"` on that line only; the
elapsed clock is `aria-hidden`). A one-second ticker updates the clock and
stops itself when no clock is on the page. With `prefers-reduced-motion` the
spinner is a still ring with a dot.

## "The pull-request metadata changed"

Every merge and update pins three object ids: the base tip, the head, and
the tip of the `forkmesh/pulls` ledger branch (`expectedPullsOid`). The
mirror refuses with `stale_pull_metadata` when the ledger tip moved, and the
ledger moves for *any* pull activity in the repository: a comment, a review,
another pull's intake. In the reported case the pull's author commented 58
seconds before the owner pressed merge.

Two things kept it failing:

1. The page pinned an old ledger tip and kept it. The pull page's read
   bundle (`app/host/forkmesh_host/repo_views.py`, `/page-reads`) is served
   stale-while-revalidate for up to `BUNDLE_STALE_SECONDS` (five minutes),
   and its `forkmesh/pulls` read seeded the page even after a reload. An idle
   page's activity tick only watches agents, so it never re-read the ledger.
2. *Retry merge* resent the same request id. The relay keeps one terminal
   answer per request id (`repo_merge_jobs`, idempotency), so a retry could
   only read back the stored failure.

Now:

- Before sending, the page re-reads the ledger tip, the pull record, its
  reviews and the merge trial with `fresh` reads that skip the page bundle,
  the early reads and the session cache (`refreshRepoPullMergePins`,
  `fetchRepoJson(..., { fresh: true })`), and pins what it just read.
- When a mirror still answers `stale_pull_metadata` or `stale_base`, the page
  re-reads once more and retries once with a new request id.
- It compares what decides the merge before and after (`pullMergeFingerprint`:
  status, head, patch series, conflicts, requests for changes). If any of
  those changed, nothing is sent and the box lists what changed; a comment,
  another pull's activity or a moved base that still merges cleanly is not a
  reason to stop.
- A request id is reused only for an answer that is not final yet (the
  relay could not be reached, the mirror is still working, or the result is
  pending), so retrying such a request can never merge twice. After a final
  answer the next attempt is a new request.

The same applies to Update branch.

## Repository tab strip on phones

The tab strip (Code, Issues, Pull requests, ...) scrolls sideways on a
phone. It used to jump:

- Re-rendering the repository page (`renderRepoDetail`, for example when the
  viewer's permissions arrive and add Agents or Settings) rebuilt the strip
  and reset its `scrollLeft` to 0. The page now carries the strip's
  `scrollLeft` across the re-render, including from the server-rendered first
  view.
- The selected tab is bold, which widened it and pushed every tab after it.
  Each label now reserves its bold width with a hidden bold copy
  (`[data-repo-tab-label]::after`).
- Selecting a tab that is off screen now scrolls the strip just far enough to
  show it (`keepRepoActiveTabVisible`), and only when the selection changed,
  never on a re-render the reader did not ask for.
- The Discussions counter is kept as invisible space while a positive count
  waits for a live mirror, instead of appearing later; counters use tabular
  figures and are only rewritten when the number changes.
- The strip contains its overscroll, hides its scrollbar, and gives icons a
  fixed box before the icon script replaces them.
- The server-rendered strip (`_tabs` in `app/host/forkmesh_host/repo_ssr.py`)
  draws the same tabs as the dashboard (including Security), the same counter
  pills (`...` until a count is known) and the same label markup, so the
  dashboard replacing it does not move anything.
