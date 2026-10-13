# Repository views

ForkMesh counts unique views of public repositories. The dashboard Home's
"Top repositories" rail lists the most recently viewed ones first.

## What counts as a view

Opening a repository page in the web dashboard (`renderRepoDetail`) sends
`POST /api/repo/<owner>/<repo>/view` once per page load
(`recordRepositoryView`, `app/public/dashboard/js/06-repo-content.js`).

- **Viewer:** a signed-in account is one viewer on every device. An
  anonymous visitor is identified by the edge address (`cf-connecting-ip`)
  plus a coarse user-agent category, the same approach as published-note
  reads. A request with neither identity is not counted.
- **Unique view:** a viewer counts at most once per 24 hours
  (`REPO_VIEW_WINDOW_MS`), measured from the last view that counted. Reloads
  and repeat visits inside that window only increase `repo_views.hits`.
  They don't change the totals or the repository's place on Home.
- **Repositories:** only public repositories are tracked. A private,
  unpublished or unknown repository answers 404 and nothing is stored.
  Organization paths are counted under the organization name, the same as
  stars (`_public_repo_owner`).

## Storage

Both tables are keyed on `blind_index("<owner>/<repo>")`. No names, addresses
or user agents are stored.

- `repo_views` (migration 0234) has one row per repository per viewer.
  `viewer_key` is a per-repository digest
  (`repo-view-v1\n<owner/repo>\n<account or visitor>`), so one viewer can't
  be linked across repositories. `counted_at` is the last view that counted.
- `repo_view_totals` has one row per repository:
  - `views`: unique views;
  - `viewers`: distinct viewers;
  - `last_viewed_at`: time of the most recent unique view.

  It is updated only when a view counts, so Home reads one small row per
  repository and never scans `repo_views`.

Deleting a repository purges both tables (`_REPO_SCOPED_TABLES`). The rekey
tool recomputes `repo_bi` and pseudonymises `viewer_key` (`docs/REKEY.md`).

## API

- `GET /api/repo/<owner>/<repo>/view` returns
  `{views, viewers, lastViewedAt}`.
- `POST /api/repo/<owner>/<repo>/view` records a view and returns the same
  totals plus `counted`. It takes an optional `sessionToken` in the JSON body.
- `POST /api/repo-views` with `{"repos": ["owner/name", ...]}` returns
  `{views: {"owner/name": totals}}`. It takes up to 200 keys and queries them
  50 at a time. It leaves out any key whose catalog row is private. Keys with
  no views are left out too.

All three answer `no-store`.

## Home ordering

`renderHomeRepositories` sends the rail's public keys to `/api/repo-views`.
It does this once per distinct key set, with no timer. `homeRepositoryEntries`
then sorts by `lastViewedAt`, newest first, and keeps the earlier order (org
repositories, then the catalog by `updatedAt`) for ties and for repositories
never viewed. The cut to 8 happens after the sort. Entries are still unique
by display key. A view counted in this tab updates the local totals at once,
so going back to Home shows the new order without another request. Each row
with views shows its unique view count.
