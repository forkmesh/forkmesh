# Repository views

ForkMesh counts unique views of public repositories; the dashboard Home's
"Top repositories" rail lists the most recently viewed first.

Opening a repository page sends `POST /api/repo/<owner>/<repo>/view` once per
load. A viewer (an account, or an anonymous edge address plus a coarse
user-agent category) counts at most once per 24 hours
(`REPO_VIEW_WINDOW_MS`). Only public repositories are tracked; others return
404 and store nothing.

## Storage

Keyed on `blind_index("<owner>/<repo>")`; no names, addresses or user agents
are stored. `repo_views` has one row per viewer with a per-repository
`viewer_key` digest (viewers cannot be linked across repositories);
`repo_view_totals` holds `views`, `viewers` and `last_viewed_at`. Both are
purged with the repository and rekeyed by the rekey tool
([REKEY.md](../REKEY.md)).

## API (all `no-store`)

- `GET /api/repo/<owner>/<repo>/view` → `{views, viewers, lastViewedAt}`
- `POST /api/repo/<owner>/<repo>/view` → totals plus `counted`
- `POST /api/repo-views` `{"repos": [...]}` (up to 200) → `{views: {...}}`
