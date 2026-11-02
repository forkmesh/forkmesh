# Pull request merge box, quick replies and conflict pings

Part of the [App docs](README.md). The conversation tab of an open pull
request ends in one merge box (`renderRepoPullMergeBox` in
`app/public/dashboard/js/06-repo-content.js`, styles `.fm-merge-*` in
`app/public/dashboard/shell.html`) that tells the whole story in one place:
readiness, the merge control and the update button. This page covers the box,
merged and closed pulls, the review chip, Update branch, live progress, the
quick review replies inside the box, the review and conflict pings that tell
an author what to do next, and the repository tab strip on phones. The
**Diff and files** box is in [pull-review-prompt.md](pull-review-prompt.md).

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

## People in the pull request list

Each pull row stacks the avatars of everyone involved, latest first: whoever
commented or reviewed last is the left face, drawn on top, and the author
closes the stack unless they spoke since. Hovering the stack lists the names
in that order. The same listing carries `pullPeople`, `{"<n>": [authorName, ...]}`:
people are accounts, never the signing key in `author` (one person signs
from several keys), so an event with no `authorName` adds no one. It is
built in the same pass as `pullReviews` from every `NNNN-*.md` event whose
type names a comment or review, latest file first with repeats dropped.
Only `NNNN-review.md` files count toward `pullReviews`: a comment file
whose front matter says `type: review` adds its author to `pullPeople`
and nothing else.
Comment files are many, so `pullPeople` has its own size check: when they do
not fit the review bounds the listing leaves `pullPeople` out (rows show the
author alone) and still carries `pullReviews`. `renderPullRowPeople` in
`06-repo-content.js` and `pull_people` in `repo_views.py` draw it.

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

## Merge from Files changed

A maintainer usually decides while reading the diff, so the Files changed tab
ends with a merge bar (`renderRepoPullFilesMergeBar`, slot
`[data-repo-pull-files-merge-slot]`, styles `.fm-merge-files`). It is not a
second merge implementation: it renders the same `repoPullMergeModel` and the
same footer as the box (`renderRepoPullMergeFooter`: footer text, merge
method, *Merge without review*, *Merge pull request*), and its buttons carry
the same `data-repo-pull-merge` / `data-repo-pull-merge-override` attributes,
so the one click handler runs `handleRepoPullMerge` with the same pins,
re-pinning, review rule and owner override. Rows that are not green (changes
requested, review required, failing or running checks, conflicts, unmet
rules) are listed above the footer as chips; *Merge details* opens the full
box on Conversation.

`updateRepoPullMergePanel` repaints both, and `patchRepoPullMutationProgress`
patches the live progress block in both slots, so the step list, timer and
result read the same on either tab. Visitors who cannot merge see no bar: the
diff stays a review surface for them.

## Live state arrives after the first paint

The pull page paints once the record, diff, conversation and commits are read;
agent activity, merge permission, readiness, merge base, checks, rules and
required checks follow (`loadRepoPullLiveState`, `finishRepoPullLiveState`)
and are patched into their slots. Until then `parsed.pullPending` is set and
the model shows "Checking mergeability…", "Loading checks…" and "Loading merge
readiness from the live mirror…", with no merge or override button, so nothing
can be merged on partial state. The header button reads "Checking…".

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

## Quick replies and conflict pings

Two things tell a pull request's author what
to do next without anyone writing it out:

- **Quick replies**: one-tap review buttons in the merge box for reviewers and
  maintainers ("Please add tests for this change.", "Merge conflict with main
  — please rebase or merge main and push again: ...").
- **Conflict pings**: when a pull request starts to conflict with its base,
  its author is pinged once, with the commands that fix it, and sees a banner
  on the pull request page.

Code: `app/src/pull_attention.py` (relay), `renderRepoPullQuickReplies`,
`sendRepoPullQuickReply`, `renderRepoPullConflictBanner` and
`hydrateRepoPullAttention` in `app/public/dashboard/js/06-repo-content.js`,
the event wiring in `08-repo-detail-network.js`, styles `.fm-merge-quick`,
`.fm-quick-reply*` and `.fm-conflict-*` in `app/public/dashboard/shell.html`,
migration `0284_pull_attention`. Tests: `app/tests/test_pull_attention.py`,
`app/tests/test_pull_quick_replies_frontend.py`.

## Quick replies

### Who sees them

A signed-in account that is not the pull request's author (by name or by this
browser's signing key), on an open pull request, when it is one of:

- an organization owner who can merge it (`pullMergeAuthorized`), an
  administrator, or the account that owns the repository's node;
- a collaborator with `triage`, `write`, `maintain` or `admin` permission
  (`/viewer-permission`);
- someone who already reviewed this pull request.

Visitors and the author see nothing. The relay accepts a review from anyone
signed in; this only decides where the buttons show.

### The set

Missing `.forkmesh/review-replies.json` means the defaults:

| id | Shows when | Review | Text |
| --- | --- | --- | --- |
| `conflict` | the merge trial reports conflicts | changes requested | Merge conflict with {base} — please rebase or merge {base} and push again: {commands} |
| `behind` | it merges cleanly but is behind | changes requested | This branch is {behind} behind {base} — please update it: {commands} |
| `checks` | a check failed on the head | changes requested | Please make the checks pass (see the Checks tab: {checks}). |
| `ai-model` | the repository requires an AI disclosure and none is stated | changes requested | Please fill in the AI model(s) used ... |
| `tests` | always | changes requested | Please add tests for this change. |
| `issue` | always | changes requested | Please link the issue this fixes in the description (e.g. "Fixes #123"). |
| `split` | always | changes requested | Please split this into smaller pull requests ... |
| `screenshot` | always | changes requested | Please add a screenshot or a short recording of the change. |
| `contributing` | a contributing guide exists | changes requested | Please follow the contributing guide: {contributing} |
| `thanks` | always | approved | Thanks! Looks good. |

`{commands}` is the exact fix for this pull request: for a branch-backed pull
`git fetch origin && git switch <head> && git merge origin/<base>`, fix,
commit and `git push`; for a patch-backed pull from someone else's copy, fetch
the base from this repository's clone URL, `git rebase FETCH_HEAD`, and submit
again (nobody here can push to their copy, see
[Update branch](#update-branch)).
The branch, base and clone values are shell-quoted when a command is copied.

`ai_missing` reads `repoCollabSettings.requireAiDisclosure` and the pull's
stated model through `pullAiModel`: the `aiModel` field or an "AI model:" line
in the description, as the disclosure chip does (reading only the field
offered the reply on pulls that stated their model in the description). On a
build without AI disclosure the setting is absent, so the reply never shows.

### `.forkmesh/review-replies.json`

Read once per page from the base commit the merge trial resolved (or the base
branch), in the same `blobs` request that looks for `CONTRIBUTING.md`,
`.github/CONTRIBUTING.md` and `docs/CONTRIBUTING.md`. Either a list or
`{"replies": [...]}`; the file replaces the defaults, and `{"replies": []}`
turns quick replies off.

```json
{
  "replies": [
    {"id": "tests", "emoji": "🧪", "label": "Add tests",
     "body": "Please add tests under tests/ for this change.",
     "state": "changes_requested", "when": "always"},
    {"id": "rebase", "emoji": "⚠️", "label": "Rebase",
     "body": "Conflicts with {base}: {commands}", "when": "conflict"},
    {"id": "lgtm", "emoji": "🎉", "label": "LGTM", "body": "Thanks!",
     "state": "approved"}
  ]
}
```

- `id`: `[a-z0-9][a-z0-9-]{0,39}`, unique; duplicates are skipped.
- `label`: up to 40 characters; `body`: up to 500, one line (whitespace is
  collapsed); `emoji` (or `icon`): up to 8 characters, optional.
- `state`: `changes_requested` (default), `commented` (sent as a plain
  comment) or `approved`.
- `when`: `always` (default), `conflict`, `behind`, `ai_missing`,
  `checks_failing` or `contributing`.
- Placeholders: `{base}`, `{head}`, `{number}`, `{author}`, `{behind}`,
  `{commands}`, `{checks}` (the Checks tab URL), `{contributing}` (the guide's
  URL).
- At most 12 replies. Invalid entries are dropped; a file that is not JSON or
  has no list counts as missing.

### Sending

One tap signs and sends the reply with the same signed pull event as the
review form (`submitWebPullEvent`, `review` with the reply's state, or
`comment`), so it lands in the ledger exactly like a typed review and counts
for the review gate. The pencil next to each reply, or pressing and holding
it (550 ms, cancelled by moving more than 10 px or scrolling), opens an
editor with the text filled in; Escape cancels, Ctrl/Cmd+Enter sends. The
editor's text and caret survive the merge box's periodic re-render.

Quick replies always go to the relay route, not to a direct mirror, because
the relay is what prevents repeats and pings the author. The request carries
`quickReply: {id, head}` beside the signed event (not inside it; it is not
part of the signature and is not stored in the ledger).

On a build with comment reactions ([comment-reactions.md](comment-reactions.md)),
an approving quick reply also adds the Thank you sticker to the pull request's
description (`sayCommentThanks`, once per viewer).

### One per commit

The relay claims `(repository, pull, reviewer, head commit, reply id)` in
`pull_quick_replies` before queuing the event. A second send of the same
reply on the same head answers `409 quick_reply_sent` and queues nothing. The
reviewer is the session's account, or the signing key without one.

The page shows a sent reply as a dashed chip with a check and "Sent". It knows
from three places: what it sent itself, `localStorage`
(`forkmesh.pullQuickReplies.sent`, the newest 200 keys), and
`GET /api/repo/<owner>/<repo>/pulls/<n>/attention?head=<sha>` (the signed-in
account's sent ids for that head, read once per head). A new push is a new
head, so every reply is available again.

## Pinging the author

Every review sent through the relay's pull inbox (`pulls_handler`), quick or
typed, pings the pull request's author with kind `pull_attention`:
"carol requested changes on your pull request #7" / "approved" / "commented
on", the review text as the body, linking to the pull request. The author is
left out of the follower fan-out (`notify_subscribers(..., skip=...)`) so
they get one ping, not two. No ping when the reviewer is the author or the
pull is closed. The author is the account bound to the pull's signing key
(`_git_push_account_for_pubkey`), else its recorded `authorName`.

## Conflict pings

### Where a conflict is noticed

The merge trial is the mirror's `merge-preview`
([merge-executor.md](../server/merge-executor.md)), which the pull page already
runs. The relay keeps one row per open pull
request it has seen in `pull_merge_watch` and re-runs the trial when
something moved:

- a pull request submitted to the relay is registered (base, head, author);
- a pull page view by a signed-in account reports the trial it ran
  (`POST .../pulls/<n>/attention {baseOid, headOid, clean, base, head,
  derive}`). A report whose base and head match the last check costs one
  read and nothing else. A reported conflict is re-checked at once by the
  relay itself (the page's word is never trusted); a clean report only marks
  the row due. Reports are rate limited to one inline check a minute per
  pull;
- a push through the relay (`record_accepted_push`) marks due every watched
  pull whose base or head branch moved (at most ten branches a push);
- a desktop publication that moves the default branch does the same for that
  branch (`catalog_routes`);
- a merge from the web drops the merged pull and marks due the pulls on its
  base; an Update branch marks that pull due;
- every watched pull is re-checked at least every twelve hours.

The `pull-attention` cron phase (even minutes) checks at most two due pulls per
run, oldest first, and stops when the cron run's fetch budget is spent
(`_cron_budget_exhausted`; every read is charged like any other cron fetch). One check reads the pull record (`pull.md`, at most three refs), the
branch list, and, only when the base or head commit changed since the last
check, `merge-preview`. A record that cannot be read backs off 10 minutes,
30 minutes, 2 hours, 6 hours, then waits for the next push, report or
twelve-hour sweep. Rows untouched for 30 days and quick reply claims older
than 90 days are pruned hourly.

### Who is pinged, and when

When a check finds a conflict, the author is pinged once per `(pull, base
commit, head commit)`: the row's `notified_bi` is compare-and-set to that
pair before anything is filed, so two cores or a report racing the cron
cannot both ping. The dedupe key is `pull-conflict:<owner>/<repo>#<n>:<base>:<head>`.
A new commit on either side that still conflicts pings again; the same pair
never does.

- Never for a draft (`status: draft` or `draft: true`) or for a pull that is
  closed, merged or deleted (its row is deleted).
- Title: "Pull request #7 has conflicts with main". Body: "Your pull request
  #7 has conflicts with main; please rebase or merge main and push again.",
  up to five conflicting files, and the git commands, as in the banner.
- Kind `pull_attention` ("Reviews and conflicts on your pull requests", in
  the Repositories group of Pings settings, on by email). It is an immediate
  kind, so a digest account is mailed straight away; a ping for a pull that
  closed before the mail went out is not mailed
  (`NOTIFICATION_PULL_STATE_KINDS`). The phone banner uses ⚠️ for a conflict
  and 📝 for a review (`push_icons.alert_icon`). Muting the kind, quiet hours
  and pauses work as for every other kind.

Private repositories are not watched: the report endpoint answers 404 and the
relay cannot read their records without the owner's key.

### The banner

The author (by account name or this browser's signing key) of an open pull
whose merge trial reports conflicts sees a red banner above the tabs: "Your
pull request has conflicts with main", what to do, up to five files, the
commands in a scrollable block, and a Copy commands button. It is a labelled
region, not an alert, so the activity refresh does not re-announce it, and it
is only rewritten when its content changes.

## Quick reply and conflict data

```
pull_merge_watch (repo_bi, number, base_bi, head_bi, due_at, checked_at,
                  trial_bi, notified_bi, updated_at, data)  PK (repo_bi, number)
pull_quick_replies (repo_bi, number, reviewer_bi, head, reply_id, created_at)
                  PK (repo_bi, number, reviewer_bi, head, reply_id)
```

`data` is `encrypt_row` of the owner, repository, public owner, base, head,
derive, author key and name, and the last trial (state, commits, behind,
ahead, conflicting paths). `base_bi`/`head_bi` are blind indexes of
`pull-watch-branch:<repo_bi>:<branch>`, `trial_bi` and `notified_bi` of the
commit pair. Repository deletion removes both tables' rows
(`_REPO_SCOPED_TABLES`). A `DATA_KEY` rotation empties both
(`tools/next_rekey.py`, [rekey](../operations/rekey.md)): watch rows come back with the next
push, submission or page view, and a quick reply already sent on a head can
be sent once more.

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
