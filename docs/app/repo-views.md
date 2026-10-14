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

## Profile repository list and popular cards

`/dashboard/profile/repositories` and `/@name/repositories` follow GitHub's
repositories tab (`profileRepositoryRow` in
`app/public/dashboard/js/05-repo-list-explorer.js`): a flat list split by
hairline borders, no card boxes. Each row has the name as a 20px accent link,
an outlined Public/Private label, the description when there is one, and a
12px meta line: language dot and name, star count, licence, "Updated ...".
On the right are the Star button and, at `lg` and wider, a green 155x30
sparkline of the last 52 weeks of commits.

Only real data is shown. The language comes from the repository's logo
metadata language weights, then its `language` field; the licence only when
the catalog carries one; the star count only once the star lookup returns a
count above zero (the button's own count is hidden and mirrored into the
meta line by a `MutationObserver`, so starring updates it in place); the
sparkline only when the activity series has commits. Fork counts and
GitHub's Lists caret are left out: the catalog has no fork count and
ForkMesh has no star lists. The old row's repository logo and default
"TypeScript"/"MIT License" values are gone. `repoLanguage(repo)` is shadowed
by the file finder's `repoLanguage(path)` in the same bundle scope, so the
row uses its own `profileRepositoryLanguage(group)`.

The organization overview's "Popular repositories" cards
(`organizationPopularCard` in `04-account.js`) follow GitHub's pinned cards:
bordered box, repository icon, name link, visibility label, description, then
language and star count. Star counts are fetched once per repository and
cached for the page's lifetime.

## Event-only repository UI

Banners, menus, dialogs and empty states on the repository pages use the
shared component classes from `app/public/dashboard/shell.html` rather than
per-page colours:

- Notices are `fm-flash` variants: blue info, amber `fm-flash-warning`, red
  `fm-flash-danger`, green `fm-flash-success`, grey `fm-flash-neutral`. This
  covers the Agents tab setup and error notices (whose runner and desktop
  links are now `fm-btn-sm` buttons and blue `text-accent` links, not the old
  green `text-primary` styling), the pull-request readiness and merge panels,
  the "rules could not be checked" and draft-review bars, the issues
  "couldn't be read" bar, the Actions notice, pending and approval bars, the
  mirror sync status and the insights "mirror unavailable" note, and the
  issues contribution tip (its waving-hand emoji is gone).
- Menus are `fm-popover` with `fm-menu-item` rows: the branch switcher (the
  current branch gets the blue check through `aria-checked`), the file-tree
  context menu, the New issue and Dismiss menus. The Code, Watch, Fork and
  issue-picker panels and the Go to file finder use the `fm-popover` surface;
  the picker's selected check is blue.
- The branch merge dialog uses `fm-dialog` inside the native `<dialog>`; the
  `fm-dialog` class sits on the inner form because its `display: flex` would
  otherwise override the closed dialog's `display: none`.
- Empty and error states are `fm-blankslate`: an empty or missing path, an
  unreachable tree, a file that will not load, empty issue and pull-request
  lists, discussions with no mirror, the Actions empty states and the
  repository list outage.
- Status colours use the `success`, `attention` and `destructive` tokens
  instead of raw emerald, amber, yellow and red utilities; badges are
  `fm-label` variants and toolbar buttons are `fm-btn fm-btn-sm`. Diff line
  tints and syntax-highlighting colours are left as they were.
- Delete confirmations still use the browser's `window.confirm`, which the
  page cannot style; replacing them with dialogs would change behaviour the
  browser tests rely on.
- `showErrorNotice` is restyled with Tailwind utilities, not `fm-toast`:
  `test_dashboard_error_actions.py` requires its classes to exist in the
  built `tailwind.css`, and the `fm-*` classes live in `shell.html`.
