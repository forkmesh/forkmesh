# Issue views

An issue page shows how many people have looked at it ("12 views") in the
header line next to the comment and participant counts, and the issue list
shows the same count in each row's meta line. It follows the repository view
count ([repo-views.md](repo-views.md)) and reuses its helpers.

## Counting

Opening an issue in the dashboard sends
`POST /api/repo/<owner>/<repo>/issues/<number>/view` once per page load
(`recordIssueView` in `app/public/dashboard/js/06-repo-content.js`, after the
issue record has loaded and rendered). That is the issue's API record being
opened by a person: the request comes from the page script, so crawlers that
fetch HTML do not send it.

- A viewer counts at most once per issue per 24 hours (`REPO_VIEW_WINDOW_MS`).
  A viewer is a signed-in account, or an anonymous edge address plus a coarse
  user-agent category, exactly as for repositories (`_repo_viewer_key` with
  the `issue-view-v1` scope).
- Requests whose user agent is empty or looks automated (`bot`, `crawler`,
  `spider`, `preview`, `curl/`, `wget/`, `python-`, `go-http-client`,
  `scanner`, `headless`; `ISSUE_VIEW_AUTOMATED_AGENTS`) are answered with the
  totals but not counted.
- Only public repositories are tracked; a private one answers 404 and stores
  nothing.
- Signed reads of the issue file itself (mirror `blob` reads used by the
  desktop, the MCP server and other tools) are not counted: they cannot tell a
  person from a program.

## Storage

Migration `0287_issue_views.sql` (down migration in `migrations/down/`) adds
two tables, keyed like the repository ones on
`blind_index("<owner>/<repo>")` plus the issue number:

- `issue_views (repo_bi, number, viewer_key, first_at, last_at, counted_at,
  hits)`: one row per viewer, `viewer_key` a per-issue digest, so a viewer
  cannot be linked across issues or repositories;
- `issue_view_totals (repo_bi, number, views, viewers, last_viewed_at)`.

No names, addresses or user agents are stored. Both tables are deleted with
the repository (`_REPO_SCOPED_TABLES`) and rekeyed by the rekey tool
([../REKEY.md](../REKEY.md)). New tables were used instead of `repo_views`
because those rows are keyed on the repository alone: issue rows there would
survive a repository purge and could not be rekeyed.

## API (all `no-store`)

- `GET /api/repo/<owner>/<repo>/issues/<number>/view` gives
  `{views, viewers, lastViewedAt, number}`.
- `POST` on the same address gives the totals plus `counted`.
- `GET /api/repo/<owner>/<repo>/issue-views?numbers=1,2,3` (up to 100 numbers)
  gives `{views: {"1": {...}}}` for the issues that have a count; the issue
  list asks for its visible page once a minute at most
  (`loadIssueListViews`).

A count of zero is not shown. The server-rendered first view of an issue page
(`issue_views_html` in `app/host/forkmesh_host/repo_ssr_issue.py`) draws the
same hidden slot, so it stays identical to the dashboard's markup; the count
appears once the page script has recorded the view. Tests: `app/tests/test_issue_views.py`.
