# Quick review replies and conflict pings

Two things that tell a pull request's author what to do next without anyone
writing it out each time:

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
migration `0280_pull_attention`. Tests: `app/tests/test_pull_attention.py`,
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

Visitors and the author see nothing. The relay still accepts a review from
anyone signed in, as it always has; this only decides where the buttons show.

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
again (nobody here can push to their copy, `docs/app/pull-merge-box.md`).

`ai_missing` reads `repoCollabSettings.requireAiDisclosure` and the pull's
`aiModel`; on a build without AI disclosure the setting is never present, so
the reply never shows.

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
typed, now pings the pull request's author with kind `pull_attention`:
"carol requested changes on your pull request #7" / "approved" / "commented
on", the review text as the body, linking to the pull request. The author is
left out of the follower fan-out (`notify_subscribers(..., skip=...)`) so
they get one ping, not two. No ping when the reviewer is the author or the
pull is closed. The author is the account bound to the pull's signing key
(`_git_push_account_for_pubkey`), else its recorded `authorName`.

## Conflict pings

### Where a conflict is noticed

The merge trial is the mirror's `merge-preview` (`docs/server/merge-executor.md`),
which the pull page already runs. The relay keeps one row per open pull
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
run, oldest first, and stops as soon as the phase's subrequest budget is
spent (`_cron_budget_exhausted`; every read is charged like any other cron
fetch). One check reads the pull record (`pull.md`, at most three refs), the
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

## Data

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
(`tools/next_rekey.py`, `docs/REKEY.md`): watch rows come back with the next
push, submission or page view, and a quick reply already sent on a head can
be sent once more.
