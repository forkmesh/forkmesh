# Recent pushes banner

A signed-in account that pushed a branch in the last hour sees a notice bar,
like GitHub's:

> **fix-login** had recent pushes 4 minutes ago  [Compare & pull request] [×]

Code: `app/src/recent_pushes.py`, `app/public/dashboard/js/09-recent-pushes.js`,
the `[data-recent-pushes]` slot in `app/public/dashboard/partials/main.html`,
migration `0279_recent_pushes`. Tests: `app/tests/test_recent_pushes.py`,
`app/tests/test_recent_pushes_frontend.py`.

## What counts

A branch shows when the account itself pushed it (`record_accepted_push`, so
owner pushes, collaborator pushes and contributor `pr/<account>/*` pushes with
the HTTPS push credential all count) within the window, and:

- it is not the repository's default branch, a tag, or a `forkmesh/*` ledger
  branch;
- it was not deleted since: a push that deletes the branch, or a branch
  delete from the web (`_branch_delete_create`), removes its row;
- no pull request was opened for it (see below);
- the viewer did not dismiss it for the same head commit.

At most three show, newest first. The window is 60 minutes;
`RECENT_PUSH_WINDOW_MINUTES` (1 to 1440) changes it.

## Forks

A push to a fork compares back to the parent: the base is the parent's
default branch, and the target is the parent. The button opens the fork's own
compare page (`/<you>/<fork>/compare/<parent default>...<branch>`), because a
compare read needs both branches on one repository, and the new pull request
form there already sends to the parent (`pullRequestParent`). When the parent
is a read-only mirror, the pull request is opened on the fork, as the form
does.

## Where it shows

On the dashboard home, the repositories list, and the repository pages of the
pushed repository and (for a fork) its parent. Not on the compare page of the
branch itself, and never to a signed-out visitor. The bar sits at the top of
`<main>`, above the page, in `fm-flash fm-flash-warning` with a plain
`fm-btn fm-btn-sm` button: the green `cta` is kept for each page's single
create button (`app/public/dashboard/theme.json`), and up to three bars can
show at once.

## Data and cost

Table `recent_pushes`, one small row per (account, repository, branch):
`account_bi`, `repo_bi`, `branch_bi`, `pull_bi` blind indexes, `pushed_at`,
`pulled`, and `data` encrypted with `encrypt_row` (public repository name and
path, branch, head commit, base, target). A new push to the same branch
replaces its row. Every recorded push deletes rows older than a day, so the
table holds at most a day of pushes. Repository and account deletion remove
their rows (`_REPO_SCOPED_TABLES`, `_delete_account_namespace`).

A `DATA_KEY` rotation (`tools/next_rekey.py`, `docs/REKEY.md`) empties the
table: `branch_bi` and `pull_bi` are blind indexes of prefixed strings, and
`pull_bi` has the target's `repo_bi` inside it, so there is no plaintext to
rebuild them from. Banners come back with the next push. A new `*_bi` table
must be registered there too, or the deploy's
`app/tests/host/test_next_rekey.py` gate fails, as it did for the 0279
migration on 2026-10-21.

Writing happens on a push only: one catalog read for the repository (plus one
for the parent of a fork, and its read-only check), one upsert per branch (at
most ten per push) and the pruning delete.

`GET /api/me/recent-pushes` is the only read: the session lookup, one indexed
`SELECT ... LIMIT 10` on `(account_bi, pushed_at)`, and a decrypt per row. No
Git or mirror read. A signed-out request gets `200 {"pushes": []}`.

```json
{"ok": true, "signedIn": true, "windowMinutes": 60, "pushes": [
  {"id": "<repo_bi>:<branch_bi>", "repo": "you/app", "repoUrl": "/you/app",
   "branch": "fix", "branchUrl": "/you/app/tree/fix", "head": "<sha>",
   "pushedAt": 1790000000000, "base": "main", "fork": false,
   "target": "you/app", "targetUrl": "/you/app",
   "compareUrl": "/you/app/compare/main...fix"}]}
```

The dashboard asks for it only on the pages above, only when signed in, once
per page load at most, after first paint (`requestIdleCallback`). The answer is
kept in `localStorage` (`forkmesh.recentPushes`, keyed by account) for a
minute: a page opened within that minute paints the bar from it straight away
and makes no request. The cache is dropped when nobody is signed in. A bar
that arrives from the network after paint does push the page down once; on
later pages it is painted from the cache at boot.

## Pull requests

Opening a pull request hides the branch in three ways, none of which reads the
pull ledger (`forkmesh/pulls`), which takes many reads per repository:

- the canonical pull route (`pulls_handler`, a new pull) sets `pulled` on rows
  whose `pull_bi` matches the target repository and head (`<branch>`, or
  `<fork owner>:<branch>` for a fork);
- the web form, whichever route delivered the pull, calls
  `POST /api/me/recent-pushes {"id": ...}`, which sets `pulled` on the
  account's own row;
- the Pull requests tab, which loads the open pulls anyway, does the same for
  any banner branch that is the head of an open pull there.

A pull opened outside the web (desktop, a mirror's own API) is not seen until
the Pull requests tab is opened; the bar goes after the window or on dismiss.
`pulled` stays set for later pushes to the branch, while its row lives.

## Compare & pull request

The button links to the compare page with `?expand=1`. After the comparison
loads, the page opens the new pull request form with base and head set and the
title filled from the newest commit's subject. `?title=` sets the title
instead; the Create pull request button on a compare page also fills it from
the newest commit.

## Dismiss

The × stores `<id>@<head>` in `localStorage`
(`forkmesh.recentPushes.dismissed`, pruned after two days); a new push has a
new head, so the bar comes back. Storage that throws is ignored.
