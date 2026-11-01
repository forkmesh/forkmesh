# Repository views

Part of [the App docs](README.md). How the dashboard and the server-rendered
first view draw repository pages: view counts, the Top repositories lists,
the title row and About rail, the issue, pull and discussion tabs, share
cards, README rendering and code addresses.

ForkMesh counts unique views of public repositories; the dashboard Home's
"Top repositories" rail lists the most recently viewed first. Opening a
repository page sends `POST /api/repo/<owner>/<repo>/view` once per load. A
viewer (an account, or an anonymous edge address plus a coarse user-agent
category) counts at most once per 24 hours (`REPO_VIEW_WINDOW_MS`). Only
public repositories are tracked; others return 404 and store nothing.

## Storage

Keyed on `blind_index("<owner>/<repo>")`; no names, addresses or user agents
are stored. `repo_views` has one row per viewer with a per-repository
`viewer_key` digest (viewers cannot be linked across repositories);
`repo_view_totals` holds `views`, `viewers` and `last_viewed_at`. Both are
purged with the repository and rekeyed by the rekey tool
([rekey](../operations/rekey.md)).

## API (all `no-store`)

- `GET /api/repo/<owner>/<repo>/view` → `{views, viewers, lastViewedAt}`
- `POST /api/repo/<owner>/<repo>/view` → totals plus `counted`
- `POST /api/repo-views` `{"repos": [...]}` (up to 200) → `{views: {...}}`
- `POST /api/repo-fork-counts` `{"forks": [...]}` (up to 24) → `{counts: {...}}`
  (see [Forks in Top repositories](#forks-in-top-repositories))

## Forks in Top repositories

Both "Top repositories" lists, the Home rail (`homeRepositoryGroup`) and the
sidebar (`sidebarRepositoryGroup`), nest forks under their parent with
`nestRepositoryForks` in `05-repo-list-explorer.js`. A row is a fork when
`repoForkParent` reads a `forkedFrom` from its catalog row; its parent is the
listed row that `repoMatchesKey` matches, or the one `findRepository`
resolves the parent key to. A fork of a fork goes under the top listed
ancestor; a fork whose parent is not listed stays a top-level row. Forks do
not use up top-level slots.

Forks sit below the unchanged parent row, indented behind a thin left border.
Each is one compact line: the owner name (or `owner/name` when renamed), the
ahead/behind badge, and on Home its view count, ordered by views. Past three
forks a "+N more forks" button expands the rest inline; it sits outside the
row links (rows are `<a>`, and a button inside would nest an interactive
element), and the expanded set lives in `state.repositoryForksExpanded`.

**New forks.** Both lists come from the public catalog snapshot, which the
cron rebuilds every few minutes and browsers cache until it expires. After
the background catalog refresh, `refreshCatalogOnNewForks`
(`08-repo-detail-network.js`) asks `/api/repo/<owner>/<repo>/forks` for each
pinned repository, once per page, and reloads the catalog live
(`loadRepositories({fresh: true})`) when the live count is higher than the
nested forks. The count it acted on is kept in `sessionStorage`
(`forkmesh.pinned-fork-count.v1:<repo>`), so a fork the catalog never lists
does not cost a live catalog read on every page.

### Ahead/behind badge

After a list renders, `hydrateRepositoryForkCounts` sends the
`data-repo-fork-counts` keys of public forks not yet asked about in one
`POST /api/repo-fork-counts {"forks": [...]}` (up to 24), cached in
`state.repositoryForkCounts` for both lists and later renders. The badge
reads `↑3 ↓12` with the title "3 commits ahead, 12 behind <parent>". It is
hidden when the count is unknown (a private fork or parent, no answer, or a
gateway that does not serve the parent) and when it is 0/0.

`app/src/fork_counts.py` requires a public catalog row for the fork, reads
the parent from that row's `forkedFrom` (never from the request), and
requires the parent to be public too. Organization addresses resolve through
`_org_repo_node`. It reads both default-branch tips from their `branches`
operations (cacheable reads, each parent once per request) and asks the
fork's gateway for
`compare?base=<parent tip>&head=<fork tip>&cherry=<parent>&counts=1`. Results
are cached per process by (fork, parent, both tips), so a moved branch gives
a fresh count. The response is `{ok, counts: {"<fork>": {ahead, behind,
parent, branch}}}`; unknown forks are left out.

`counts=1` is a mode of the gateway's upstream comparison (`cherry`): with
the parent's objects as an alternate it runs
`git rev-list --left-right --count <base>...<head>` and returns only
`{ok, baseOid, headOid, ahead, behind}`. It takes commit ids, and refuses
`limit`, any value other than `1`, and an upstream the gateway does not
serve. The App forwards `counts` for `compare` in
`_https_mirror_request_query` and `edge_routing.masked_target_url`. The
catalog's default-branch commit is only as fresh as the last publish, so the
counts read live tips.

## Pinned repositories and fork count

An instance can choose what both Top repositories lists show with the
`instance_settings` row `sidebar`:

```sql
INSERT INTO instance_settings (k, doc, updated_at)
VALUES ('sidebar', '{"pinned":["acme/app","acme/docs"],"forks":5}',
        CAST(strftime('%s','now') AS INTEGER) * 1000)
ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, updated_at=excluded.updated_at;
```

- `pinned`: repositories shown instead of the most viewed, in this order. An
  entry is `owner/name` (catalog address or the public owner on the row) or
  a bare `name`, which pins the listed repository with that name and never a
  fork of it. When several rows match, an original beats a fork, then the
  most forks wins; entries matching nothing are skipped. The sidebar shows
  only pinned rows; Home shows them too unless a search is typed or "Show
  more" is open.
- `forks`: forks per row before "+N more forks", in both lists (0 to 50,
  default 3).

`Default._instance_sidebar_setting` cleans the row for `/api/instance`
(`sidebar`): `pinned` is lower-cased, de-duplicated, at most 10 entries of at
most 161 characters with at most one `/`. A missing, unreadable or
non-object row answers `{"pinned": [], "forks": 3}`; a non-list `pinned`
counts as empty, so a lone string is not read letter by letter. The
dashboard reads it once per page through `instanceInfo` (`loadSidebarSetting`
in `05-repo-list-explorer.js`) and redraws both lists. With no row, an empty
list, or nothing matching, the lists keep their usual order (six sidebar
rows, eight Home rows). Outside an instance `instanceInfo` answers nothing.
`/api/instance` is cached for 60 seconds.

An instance admin edits the row in Settings → Top repositories
(`initInstanceSidebarSection` in `04-account.js`): one pinned name per line
and the forks count. `GET /api/instance/sidebar` answers the cleaned setting
and limits; `POST` (`{"sidebar": {...}}`) saves it. Both need an instance
admin session (`_is_admin`; 401 without a session, 403 `not_admin`
otherwise), and a cookie session is origin-checked. A write is validated
strictly (`instance_sidebar.clean`): each entry is `name` or `owner/name` in
the account and repository name grammar (`invalid_pinned`), at most 10
(`too_many_pinned`), `forks` a whole number from 0 to 50 (`invalid_forks`);
blank lines and repeats are dropped; audited as `instance.sidebar`. Reading
stays lenient (`instance_sidebar.parse`) so a hand-written row keeps working.

The row also holds `devto`, a dev.to account name (`[a-z0-9_]{1,30}`, a
leading `@` dropped on write, `invalid_devto` otherwise; `""` when unset).
When set, Home's "Latest from the blog" card shows that account's newest
posts instead of the instance blog (`loadHomeDevtoPosts`): one
credential-less read of `https://dev.to/api/articles` per Home load, keeping
only `https://dev.to` post links and cover images from `media2.dev.to` or
`dev-to-uploads.s3.amazonaws.com`, opened in a new tab. Visitors' browsers
contact dev.to only on an instance whose admin set the account.

The browser keeps the last setting in `localStorage`
(`forkmesh.sidebarSetting`) so the next page draws the pinned lists at once;
while the repository list loads, a pinned list that matches nothing yet
shows nothing, so the lists do not jump. Tests:
`app/tests/test_sidebar_setting.py`,
`app/tests/test_instance_sidebar_setting.py`.

## Contribution tips

The "Want to contribute" tip on Issues and the "First time contributing" tip
on Pull requests remember a dismissal in `localStorage`. A repository can
load under its storage owner (`drew/Widgets`) or its organization
(`acme/Widgets`), so `contributionTipKeys` lists one key per name: `repo.owner`,
each of `repoLogicalOwners`, and the storage owner of any catalog row with
the same name under one of those organizations (a dismissal saved before the
organization became the public owner still counts). Dismissing writes all of
them; the tip stays hidden if any is set. "Dismiss for all repositories" is
the single `forkmesh.issue-contribution-tip.v1:all` key.

## Left rails on every repository tab

Discussions, Insights, Actions, Security, Agents and Settings use the Issues
layout (`data-repo-rail-layout`): a rail flush against the left edge that
stays put while the page scrolls.

- **Agents:** status views (`REPO_AGENT_STATUS_VIEWS` in `06-repo-content.js`):
  all, running, finished, and failed (failed, error, stopped or cancelled),
  each with its count.
- **Settings:** `renderRepoSettingsRail` (`08-repo-detail-network.js`) lists
  one entry per `data-repo-settings-section` card, marks the card at the top
  as the page scrolls, and a search hides cards and entries whose title, text
  or placeholders do not contain every typed word.
- A repository without its own logo shows the instance logo (header
  `img.brand-mark`, same-origin path only) in place of the book icon.

## Views sidebar

The Issues and Pull requests views sidebar collapses from a
`panel-left-close` icon at its top right (`renderCollectionViewsHeader`);
when collapsed, the `panel-left-open` icon beside the page title brings it
back. Both are `data-repo-issue-views-toggle` buttons sharing one handler.

## Profile repository list and popular cards

`/dashboard/profile/repositories` and `/@name/repositories` follow GitHub's
repositories tab (`profileRepositoryRow` in
`app/public/dashboard/js/05-repo-list-explorer.js`): a flat list split by
hairline borders. Each row has the name as a 20px accent link, an outlined
Public/Private label, the description, and a 12px meta line (language dot
and name, star count, licence, "Updated ..."). On the right are the Star
button and, at `lg` and wider, a green 155x30 sparkline of 52 weeks of
commits.

Only real data is shown: language from the logo metadata language weights,
then `language`; licence only when the catalog has one; star count only once
the lookup returns more than zero (the button's own count is hidden and
mirrored into the meta line by a `MutationObserver`); sparkline only when the
series has commits. No fork count (the catalog has none) and no Lists caret
(ForkMesh has no star lists). `repoLanguage(repo)` is shadowed by the file
finder's `repoLanguage(path)` in the same bundle scope, so the row uses
`profileRepositoryLanguage(group)`.

The organization overview's "Popular repositories" cards
(`organizationPopularCard` in `04-account.js`) follow GitHub's pinned cards:
bordered box, repository icon, name link, visibility label, description,
language and star count. Star counts are fetched once per repository per
page.

## Event-only repository UI

Repository page banners, menus, dialogs and empty states use the shared
component classes from `app/public/dashboard/shell.html`
([dashboard colours](dashboard-colors.md)):

- Notices are `fm-flash` variants: blue info, amber `fm-flash-warning`, red
  `fm-flash-danger`, green `fm-flash-success`, grey `fm-flash-neutral`
  (Agents tab notices with `fm-btn-sm` buttons and blue `text-accent` links,
  pull readiness and merge panels, rules and draft-review bars, the issues
  read-error bar, Actions bars, mirror sync status, the insights "mirror
  unavailable" note, the issues contribution tip).
- Menus are `fm-popover` with `fm-menu-item` rows: the branch switcher
  (current branch checked in blue through `aria-checked`), the file-tree
  context menu, New issue and Dismiss. The Code, Watch, Fork and issue-picker
  panels and Go to file use the `fm-popover` surface. The file finder is a
  modal combobox: focus stays between its search and close controls, arrows
  move the active result, Escape returns focus to its opener, and result rows
  are not Tab stops.
- The branch merge dialog puts `fm-dialog` on the inner form of the native
  `<dialog>`, because its `display: flex` would override the closed dialog's
  `display: none`.
- Issue and pull metadata pickers are non-modal popovers: opening one focuses
  its first field; Escape or Close returns focus to the settings button.
- Empty and error states are `fm-blankslate`.
- Status colours use the `success`, `attention` and `destructive` tokens;
  badges are `fm-label` variants and toolbar buttons `fm-btn fm-btn-sm`. Diff
  tints and syntax colours are unchanged.
- Delete confirmations still use `window.confirm`: replacing them would
  change behaviour the browser tests rely on.
- `showErrorNotice` uses Tailwind utilities, not `fm-toast`:
  `test_dashboard_error_actions.py` requires its classes in the built
  `tailwind.css`, and the `fm-*` classes live in `shell.html`.
- Ctrl/Cmd+Enter in a form field submits through the form's primary button,
  found as `.bg-cta, .fm-btn-cta` (`08-repo-detail-network.js`).

## Repository title row

Fork and Star sit next to the name, as on GitHub, where people look for them
(in the About rail they read as stats and web-only visitors never found
Fork). `renderRepoTitleRow` in `08-repo-detail-network.js` draws one row with
a bottom border under the tab bar, on the Code tab only
(`data-repo-title-row` is hidden elsewhere):

- **Left:** the repository logo (`nativeRepositoryLogoMarkup`) or the owner's
  initial, the bold name, and a Public/Private `fm-label`. Under it the
  provenance line (`repoProvenanceHtml(repo, { forkButton: false })`):
  "forked from owner/repo" as an accent link, or a mirror's "Mirror of" line
  and read-only notice (with no Fork button of its own).
- **Fork N ▾:** a neutral `fm-btn-sm` split button.
  - The main part (`data-repo-fork-primary`) opens your own fork when the
    forks list or a create response names one; otherwise it opens the menu
    and creates the fork.
  - ▾ (`data-repo-action-fork`) opens the forks menu: the list, forks on
    other instances, and the page's one green button, "Create fork".
  - Creating calls `POST /api/repo/<o>/<r>/forks`
    (`createRepoForkFromWebsite` in `06-repo-content.js`), refreshes the list
    and shows "Open fork". A response with `push` shows the remote commands,
    username, password and expiry once (`repoForkPushHtml`). `exists: true`,
    or a response that is neither `created` nor `queued`, means you already
    had a fork.
- **Star N:** "Star N" or "Starred N" with the yellow star;
  `setRepoStarButtonState` sets the colour inline because `fm-btn` greys
  every icon. Stars are blind-indexed and have no list, so there is no ▾.

`renderRepoDetail` draws the Fork button in the Code toolbar too, so it
declares its own `websiteForkAvailable` (without it every repository page
stopped with a ReferenceError). Watch stays in the About rail. The forks menu
is anchored to the actions container, which `ml-auto` keeps at the right
edge even when it wraps on a phone. The server-rendered first view
(`_title_row` in `app/host/forkmesh_host/repo_ssr.py`) draws the same row
with placeholder counts, so nothing shifts when the dashboard takes over.
Tests: `app/browser_tests/tests/dashboard-repo-forks.spec.js`,
`app/tests/test_dashboard_repo_mirror_label.py`,
`app/tests/test_dashboard_landing_migration.py`,
`app/tests/host/test_repo_ssr.py`.

## Code page About rail

The About rail beside the Code tab is a details list. Under the description
and the Readme / Activity / clean-URL links, one row per fact shows a muted
label and the value: Website, Release, Watchers, Mirrors, Files and Size.
Watchers opens its popover to the right, Mirrors the Mirrors tab, Release
Releases, and Size the Size map tab beside a tiny sunburst. Rows stay hidden
until their data loads. There is no banner or logo header; the fediverse
handle and watcher list are in the Watchers popover.

Every `http(s)://` address in the description is a link (accent, new tab,
`nofollow ugc`), found the way the README renderer finds bare URLs
(`MD_BARE_URL`, `mdTrimAutolink`, `mdLinkHtml`); the rest stays escaped. A
read-only mirror without its own description links the upstream path in
"Read-only mirror of …". Long addresses wrap. The first render and a saved
edit both go through `repoAboutDescriptionHtml` in `06-repo-content.js`;
tests in `app/tests/test_dashboard_repo_mirror_label.py`.

Below the list, Languages, Contributors and Direct access are plain sections.
Contributor avatars are neutral initials (name and commit count in the
tooltip), with a picture drawn over them when one is known:

- `avatarUrl` from the gateway's `stats`: the repository's
  `.forkmesh/info.json` contributor list, else a GitHub no-reply commit email
  (`<id>+<login>@users.noreply.github.com` gives
  `avatars.githubusercontent.com/u/<id>`, `<login>@users.noreply.github.com`
  gives `github.com/<login>.png`; `[bot]` logins without an id get none).
  Only plain `https:` addresses, loaded lazily without a referrer; a failed
  picture is removed.
- otherwise a contributor name that is a valid account name is looked up
  (`/api/accounts/<name>`, the cached lookup issue actors use) and its avatar
  used when the account exists and is public. Names with spaces keep
  initials.

**Contributor roster.** A repository can ship its own list at
`.forkmesh/contributors/contributors.json`, with one avatar per contributor
at `.forkmesh/contributors/<slug>.png`:

```json
{"about": "How the list was made.", "count": 3,
 "contributors": [{"slug": "alice", "name": "Alice", "login": "alice",
   "profile": "https://example.com/@alice", "commits": 120,
   "first": "2024-01-02", "last": "2026-10-01", "projects": ["acme-api"]}]}
```

When present, Contributors shows `count` (never below the number of valid
rows) and the top 8 rows by `commits`, pictures read through the raw route at
the selected branch (`repoRawUrl`). An avatar links to `profile` only when it
is a plain `https:` URL; a missing picture keeps the initials; "+N" opens the
file. Other fields are ignored. The file is read with the optional
`.forkmesh` folder machinery below (no request unless `.forkmesh` has a
`contributors` folder; a miss is remembered), at most 2 MiB and 10,000 rows;
a slug must match `[a-z0-9][a-z0-9._-]*`. Without a valid roster the section
uses the `stats` authors minus bots (`… [bot]`, `GitHub Action(s)`, `… Bot` /
`…-bot`, `<slug>-actions`, `<platform> Actions` / `actions@forkmesh.local`
and `ForkMesh Deploy`), dropped from avatars and count. Code:
`loadRepoContributorRoster`, `renderRepoAboutRoster`, `repoContributorIsBot`
in `06-repo-content.js`; tests in `app/tests/test_repo_contributor_roster.py`.
The upstream sync that writes this roster for mirrored forums is in
[forge-sync.md](../forge-sync.md#avatars).

Commit emails are not sent to the browser. Direct access lists online
mirrors as plain rows: node name, its version or (for the node serving the
page) the response time, and an open-direct icon. The logo suggestion form
and the owner's fediverse posts are collapsibles at the bottom, then the
"Mirrors" card with the mirror goal and each copy's health
([mirror-quota.md](../mirror-quota.md); see
[mirroring](../mirroring.md)).

## Issues, pull requests and discussions

These tabs follow GitHub's layouts. The issue list's label filter lives in
the address bar (`?label=…`), so a filtered list can be linked and survives a
refresh; an open issue keeps its own address. A label's swatch is its colour
dot, except Hacktoberfest, a small pumpkin that wiggles when the list first
paints (still under reduced motion).

- **Issues and Pull requests lists** share one layout
  (`data-repo-collection-layout`): on wide screens the views sidebar is flush
  left with a right border, the list centred beside it at up to 80rem.
- **On a phone:** `shell.html` raises controls to 44px under
  `(pointer: coarse)`, but the New issue chevron is a `<summary>`, so the
  toolbar (`data-repo-collection-chrome`), list header and pagination are
  exempt and keep the 32px compact height. Below 640px the "All issues" /
  "All pull requests" heading is visually hidden (the view select names the
  view) so select, refresh and New issue fit one row; the filters (Author,
  Labels, Milestones, Assignees, Types, Sort) scroll sideways on one line
  (`data-repo-issue-list-filters`); rows use 12px side padding, 6px vertical
  padding and a 14px title. The rules are hand-written CSS on data attributes
  because the committed `tailwind.css` has no `sm:` variants for them.
  `dashboard-issue-filters.spec.js` "on a phone" checks it.
- **Pull request views** are Pull requests, Authored by me and Mentioned, then
  a Branches link and the base branches of open pulls. Pulls carry no labels,
  milestones, assignees or review requests in `pull.md`, so the filters are
  Author, Base (with more than one) and Sort, carried in the search box as
  `is:pr state:open author:… base:… mentions:@me sort:…`. Row icons: open
  green, merged primary blue, closed red (purple is not a ForkMesh colour).
- **Pull request page**: no Back button (the tab returns to the list; same
  for Issues and Discussions). The header is title and number, state pill,
  "wants to merge N commits into `owner/repo:base` from `owner/repo:head`"
  with a copy button, and boxed section tabs with the diffstat. The
  description is an avatar-left comment card; commits and bodiless reviews
  are timeline rail lines, reviews with text are cards. The sidebar lists only
  sections with real data.
- **People in timelines** (timelines, comment cards, the opening card, "set
  assignees"): names come from `issuePersonNameHtml`, faces from
  `issueActorAvatarHtml` (`02-helpers.js`). The picture is the forge
  account's avatar (`/api/accounts/<name>`, one cached lookup per name), then
  the roster picture of a row whose `login`, `github` or `gitlab` matches
  case-insensitively (`avatar` when it points into `.forkmesh/contributors/`,
  else `<slug>.png`), then initials. A name links to `/@<name>` when the
  account is linkable (a taken, public user or node account;
  [profile-links.md](profile-links.md)), else to the roster `profile` (plain
  `https:`, new tab), else nowhere. Bots (`…[bot]` and what
  `repoContributorIsBot` treats as bots) get the lucide `bot` mark and no
  link. Faces have fixed sizes and pictures carry width, height and
  `loading="lazy"`, so nothing moves. Cards keep a 40 px avatar column on
  wide screens and a 24 px avatar beside the name on phones. The roster is
  read once per repository and branch (`repoContributorRosterCached`).
  Tests: `app/tests/test_issue_timeline_people.py`.
- **Code map No wrap** (`layoutPrefs`, `fitCode` in
  `app/public/assets/codemap/codemap.js`): by default lines flow into newspaper
  columns cut at the file's 97th-percentile line length (at most 110
  characters). "No wrap" lays the file out as one column sized to the tile
  height (at most 80 characters wide) and clips lines at the tile edge
  (`code.clip`). Kept in `localStorage` (`forkmesh-codemap-wrap`).
- **Code map text vs binary** (`createLoader`, glue `openRepoCodeMap`): a file
  is a binary card only when the host answered base64 or its text holds a
  NUL byte. A path the `/blobs` batch left out (`null`: upstream timeout or
  batch byte budget) or a failed batch is retried alone up to three times,
  then left blank, never carded. The gateway cuts a blob over
  `MAX_JSON_BLOB` at a UTF-8 boundary (`_utf8_prefix`), so a cut text file
  stays text.
- **HTML entities in markdown** (`MD_ESCAPED_ENTITY` in
  `renderMarkdownInline`): text is escaped first, then, after code spans are
  parked, a well-formed named or numeric entity (`&rlm;`, `&zwnj;`) is
  restored as GitHub renders it. An entity only yields text,
  `&lt;script&gt;` stays visible text, and code spans keep the literal.
- **@mentions in markdown** (`MD_MENTION`): an `@login` (letters, digits and
  single inner hyphens, at most 39) links to its profile, resolved like a
  timeline name, or stays plain text with no account and no roster profile.
  It is matched after code spans, links, images and bare URLs are parked, and
  never when the `@` follows a letter, digit, `.`, `/`, `+` or `-` (emails,
  URL paths, `@scope/package`) or the login runs on into `.x`, `/x`, `_x`,
  `@` or `[bot]`. Task markdown opts out (`mentions: false`). Tests in
  `app/tests/test_repo_readme_markdown.py`.
- **Mention suggestions** in every record editor (comment box and its @
  button, pull reviews, discussion replies, the issue description editor,
  inline pull comments, new issue and new pull forms; a textarea opts in with
  `data-mention-input`; see [username-autocomplete.md](username-autocomplete.md)).
  `@` plus characters opens up to 8 people at the caret: people in this issue
  or pull (author, commenters, assignees, reviewers, mentioned; most recent
  first), then the roster by commits; prefix matches first, then anywhere.
  There is no forge-wide search because `/api/accounts/users` returns every
  account's picture. Tap, Enter or Tab inserts `@login ` (replacing a
  half-typed login); Escape closes; arrows move. The list is a `listbox`
  driven by `aria-activedescendant`, rows 44 px, placed in the visual
  viewport so it stays above a phone keyboard. Nothing is fetched per
  keystroke. Editing a comment opens an inline editor in its card
  (`openIssueCommentEditor`); a quiet refresh waits while it is open.
- **Development links** (GitHub's Development section on issue and pull
  pages). There is no link event type, since a new signed event would need
  every client and the gateway to learn it; links are read from signed text,
  like issue relationships:
  - A pull links issue #N when its title or body names `#N`, or a comment or
    commit message says `close`, `fix` or `resolve` (any tense) `#N`. Code
    spans and fenced blocks don't count.
  - An issue's description or comments link pull #N with
    `Linked pull request #N`, `fixed by #N`, `resolved in PR #N` and similar,
    and unlink it with `Unlinked pull request #N`. The last line about a pull
    wins, so an unlink also hides a link from the pull's text. A plain
    `fixes #N` on an issue is not a link: issues and pulls are numbered
    separately.
  - The Development settings button (any signed-in visitor; the write is an
    ordinary comment) opens the picker: on an issue it searches the pull
    list, on a pull the open issues (`loadRepoIssueRelationIndex`); typing a
    number links an unlisted one. Closing it writes one signed issue comment
    with a `Linked/Unlinked pull request #N` line per change. From a pull
    page that comment goes on each issue, because an issue page reads only
    the pull list; likewise a web pull comment saying `fixes #N` also writes
    `Linked pull request #P` on issue #N (`linkPullCommentIssues`). A comment
    of only those lines shows as a "linked pull request #P" rail line.
  - The issue page reads the pull list in the background
    (`loadRepoDevelopmentPulls`, cached a minute). Rules:
    `pullOwnIssueNumbers`, `issueDevelopmentPullStates`,
    `issueLinkedPullNumbers`, `pullLinkedIssueNumbers` in `06-repo-content.js`,
    ported to `development_pull_states` in `repo_ssr_issue.py`. The desktop
    and phone apps don't show these links yet. Tests:
    `app/tests/test_development_links_frontend.py`,
    `app/browser_tests/tests/dashboard-development-links.spec.js`.
- **Discussions** have a search box (`is:open category:…`), Sort by (latest
  activity, date created, oldest), Filter (open, closed, all) and New
  discussion, with Categories on the left. Rows show the category emoji,
  title, who started it, when, and the category; no vote or reply counts,
  because the list reads only `discussion.md`. New discussion (and the empty
  list's Start a discussion) opens the web composer; see
  [web-discussions.md](web-discussions.md).

## Reconnecting after a mirror outage

Pull, issue and discussion lists and records, projects, the code tree,
commits, branches and releases share one helper in `06-repo-content.js`
(`handleRepoReadFailure`, `finishRepoRead`). A read failing with 503
`mirror_unavailable` (or 503 without a code), 502, 504 or a network error
shows "Reconnecting to the repository host…" and retries after 2, 4, 8, 16,
then every 30 s. After two minutes the page says "The repository host is not
answering right now." with a Retry button. A 404 is the repository's answer:
a record page says the record was not found and names its file; other errors
say the view could not be loaded. The wording must not read as permanent: a
single-server instance shows it on every deploy while the gateway restarts.

Each view's last HTML is kept in `sessionStorage`
(`forkmesh.repoView.v1|<node>|<owner/repo>|<view>`, at most 24 views of
400 000 characters, oldest dropped first, every access in `try`) and shown
under a "Reconnecting…" note while the host is away. The key includes the
signed-in node name, and sign-out clears every `forkmesh.` key in
`sessionStorage`. A read answered with `X-ForkMesh-Stale: 1` (the host's
stale cache, [docs/host/README.md](../host/README.md)) shows an "Updating…"
note and is read again on the same schedule.

A retry runs only while the address and container are the ones it was armed
for; `navigateHistory` and the `popstate` handler cancel pending retries, and
a retrying loader does not repaint its loading state over the kept view.

## Issue page first view

The node draws an issue page (`/<owner>/<repo>/issues/<n>`, or
`/<name>/issues/<n>` with short addresses) into the first HTML
(`app/host/forkmesh_host/repo_ssr_issue.py`), so nothing below the title
moves when the dashboard takes over:

- **Same markup as the dashboard.** `render_issue` draws the article
  `renderRepoIssueDetail` (`06-repo-content.js`) draws for a visitor without
  write access, from the issue's signed-event JSON. Its helpers are ports
  (`escapeHtml`, `formatTimeAgo`, `parseIssueJson`, `issueDetailParsed`,
  `issueLabelHue`, the timeline people helpers, `renderMarkdown` with its
  inline rules and @mention links); icons are the `<svg>` lucide 0.468's
  `createIcons` puts in place of `<i data-lucide>`. The frame is
  `renderRepoDetail` with the Issues tab open on a record. `dashboard.js`
  keeps the article (`takeRepoFirstViewIssue`; `loadRepoRecordDetail` puts it
  where the skeleton would go), then paints its own render over it, keeping
  every server node it would draw the same way (see **Long threads**). Any
  change to the dashboard's issue markup must be made in both; the parity
  tests in `app/tests/host/test_repo_ssr_issue.py` run the dashboard's own
  functions under node and compare the renders (add any new helper to the
  test's `FUNCTIONS`/`CONSTANTS`).
- **Long threads.** The forum's requirement is that a whole thread is
  readable from the first HTTP response without JavaScript, on links as slow
  as a DNS tunnel (see [censorship resilience](../censorship-resilience.md)),
  so the first HTML carries every comment, streamed:
  - **Streamed.** `inject_issue_page` (`repo_ssr.py`) returns an `IssuePage`:
    the page through the header, opening post and first `TIMELINE_FIRST`
    (10) timeline items, then the remaining items `STREAM_CHUNK` (10) at a
    time, then the rest (sidebar, comment box, scripts). `finish` answers a
    `ReadableStream`, so the ASGI layer sends each part as it is ready
    (chunked). `render_issue_parts` (`repo_ssr_issue.py`) leaves `REST_MARK`
    where later items go and returns their render functions
    (`Renderer.timeline_items`), each chunk rendered with the forge-avatar
    context set again. The page is cut at the mark, so the mark must come
    only from the renderer: `esc` turns every NUL into U+FFFD,
    `render_issue_parts` refuses an article with more than one mark, and
    `inject_issue_page` sends the page whole if the shell or tabs hold one
    (a mark in an author name once moved the cut into an attribute: stored
    XSS on the app origin). The whole article is remembered on the bundle
    once the last chunk is sent and then goes out in one piece. A failed
    chunk ends the thread early and still closes the page; the dashboard
    draws what is missing. `inject_issue` joins the parts for string
    callers. Caddy must not hold the stream: the instance Caddyfile
    (`install-instance.sh`) and `deploy/core/render.py` set
    `flush_interval -1` on the App proxy.
  - **No competing download.** On a server-rendered issue page the head
    script's `page-reads` bundle fetch (`window.__forkmeshView`) waits for
    `DOMContentLoaded` (`_view_reads_head(meta, after_page=True)`), so the
    record does not share a slow link with the HTML. The dashboard reads the
    record once from that bundle (`seededRepoRead`) for the comment form,
    reactions and edit controls.
  - **Taking over without redrawing.** `renderRepoIssueDetail` builds the
    timeline as entries (`issueTimelineEntries`, each with a lazy `html()`)
    and draws only the first `ISSUE_TIMELINE_FIRST` (10, kept equal to
    `TIMELINE_FIRST` by a test) plus, on in-app navigation, a placeholder
    (`<div data-repo-issue-more data-remaining="N">`, two shimmer rows and
    "Loading N more comments…"). Every issue render goes through
    `paintRepoIssueDetail` (`loadRepoRecordDetail`, the optimistic view, the
    late relations redraw, `renderCurrentWebIssueDetail`): for the sticky
    bar, header, opening post, sidebar, Fediverse thread and each timeline
    item it keeps the node on screen when its signature matches
    (`issueNodeSignature`: the markup as first drawn, lucide `<svg>` turned
    back into `<i data-lucide>`, whitespace collapsed; recorded for the
    server page by `keepRepoFirstViewIssue` before avatars are filled in).
    Items past the first ten stay in front of a hidden placeholder, and the
    stream (`queueIssueTimelineRest`, `ISSUE_TIMELINE_BATCH` = 15 entries per
    `requestIdleCallback`, a 16 ms timeout without it) renders each entry and
    keeps the matching node (`placeIssueTimelineItems`), inserting only what
    differs and removing what no longer exists. A guest keeps the very nodes
    the server sent, a member sees only comments that gain edit buttons
    swapped, and the scroll position holds. Without a server page the
    placeholder shows and items are appended. Lucide draws new icons once,
    after the last batch. A newer render stops the old stream.
  - **Comment links.** Each comment card has `id="issuecomment-<event id>"`.
    A fragment `#issuecomment-<id>` (or GitHub's `#issuecomment-<number>` for
    a mirrored `forge-github-comment-<number>`) makes the dashboard draw
    through that comment and scroll to it, once per address; on the server
    page the browser finds it itself.
  - **Off-screen cards.** `shell.html` gives timeline comment cards
    `content-visibility: auto` with `contain-intrinsic-height: auto 180px`
    and `scroll-margin-top: 64px` (clear of the sticky title bar), so a
    500-comment page paints its top at once while find in page and anchors
    still reach the text. Paint containment would clip a 2 px avatar hover
    ring at the card's left edge; the reaction picker opens in
    `document.body`.
  - Posting appends after the placeholder; avatars and person links are
    filled in by the existing `MutationObserver`; edit, delete and reaction
    controls are delegated, so streamed items behave like the rest.
  - Tests in `test_repo_ssr_issue.py`: whole-thread parity (first ten plus
    streamed rest against the server page), the streamed parts and order,
    placeholder and fragment handling, and a node harness with a small fake
    DOM that runs the stream (batches of 15, order, first nodes kept,
    server nodes reused, a stale one replaced).
- **Viewer-dependent parts.** The comment box, the sub-issue Add button and
  the Development settings button are drawn in guest and member variants,
  and the head script picks one before first paint (`html[data-guest]`,
  `VIEWER_STYLE`). A member's comment-box avatar is blank: the node does not
  know who is signed in.
- **Escaping.** Everything from the issue is escaped, the state label
  included: the issue file's `status` is author-controlled and not checked
  against a signature here. The Markdown port escapes before it formats.
- **Data.** `repo_views` builds an issue bundle (the whole record and the
  issue index's `related` answer for mentions and sub-issues; the open
  records on a gateway without the index, see
  [issue-scale-limits.md](issue-scale-limits.md#the-issue-index)) from the
  local gateway, remembered per content; the rendered article is kept for a
  minute because its relative times move. The page waits at most
  `ISSUE_BUDGET_SECONDS` (0.3 s) for the issue and the repository's first
  view, otherwise it is served without them. Without a remembered `/about`, a
  feature tab's page draws the default tabs.
- **View on GitHub.** When the issue index knows the provider issue
  (`sourceUrl`), the title actions start with a link to it
  (`renderIssueSourceLink` / `source_link_html`): "View on GitHub" or "View on
  <host>", text hidden below `sm`. New tab, `rel="noopener noreferrer
  nofollow"`; both renderers accept only an `https` address of plain path
  characters. Nothing loads from the provider unless the reader follows it.
  The dashboard learns it with the relations (`loadRepoIssueRelations`, kept
  in `repoIssueSources`), the server from the bundle's `related` answer
  (`index_source_url`).
- **Order (`forum-list` plugin).** With the `OPT_IN` instance plugin
  `forum-list` on, the issue list's default sort is Newest (`newest`: created
  time descending, ties by number descending), GitHub's
  `sort=created&direction=desc`; otherwise it is "Recently updated"
  (`issueDefaultSort`). Both stay in the Sort menu; the Recent view picks
  "Recently updated". The default is not written into the search box.
  `issueSortItems` falls back to Newest for an unknown sort, and the
  server-rendered list orders rows the same way (`_created_ms`, seconds read
  as milliseconds like `stamp`). A thread shows comments and events oldest
  first by `ts` (`renderIssueTimeline` / `Renderer.timeline`, stable sorts);
  mirrored comments carry the provider's creation time.
- **Instance switches.** With the `issue-ai-prompt` or `fediverse-ui` plugin
  off ([instances](../operations/instances.md#plugins)) neither renderer draws the
  prompt box or the Fediverse thread; the server reads `plugins_off()` and
  keys its kept article by it.
- **Pictures.** A paragraph that is one picture of unknown size
  (`mdLoneImage` / `lone_image`) is drawn in a fixed 16:9 frame
  (`img[data-md-figure]` in `shell.html`, at most 24rem high), so the
  conversation below does not move when it loads.
- **Mirrored attachments.** `tools/forge_sync.py` copies the images and files
  GitHub threads embed into `.forkmesh/attachments/`
  ([forge-sync.md](../forge-sync.md#attachments)). The issue page
  reads the issue's map `.forkmesh/attachments/issues/<n>.json` with one
  `blobs` read at the branch (`loadRepoIssueAttachments`, kept in
  `repoAttachmentFiles`; `_issue_attachments` in `plan_issue` puts the same
  read in the bundle and the map in `model["attachments"]`, which
  `issue_article` hands to the `Context`). The list page reads nothing. For
  an address in the map (looked up as written, with the five `escapeHtml`
  entities undone: `mdAttachmentKey` / `attachment_key`) both renderers:
  - draw an image (`mdImageHtml` / `image_html`) from the stored file's raw
    URL at the page's branch (`repoRawUrl` / `Context.raw_url`), an SVG
    through `data-repo-svg-src`; a non-image file in an image's place
    becomes a link;
  - point a link (`mdLinkHtml` / `link_html`), bare addresses included, at
    that raw URL, `data-md-attachment`, new tab.

  With the `OPT_IN` instance plugin `mirrored-attachments` on, a GitHub
  attachment address not in the map (`MD_GITHUB_ATTACHMENT_URL`:
  `github.com/user-attachments/`, `github.com/<o>/<r>/assets|files/`,
  `user-images`, `private-user-images`, `objects.githubusercontent.com`) is
  never an `<img>`: it becomes a link reading "image on GitHub" (or "<alt>
  (image on GitHub)"), `data-md-github-image`, address in `href` and
  `title`. This applies to READMEs and the plain fallback view
  (`repo_ssr._image_html`, no map) too. Links to such addresses stay links;
  other hosts' images are drawn as before. Map paths must look like
  `.forkmesh/attachments/<sha256>.<ext>`; anything else is ignored. Parity
  tests cover a mapped and an unmapped thread.

## Pull requests for AI reviewers

AI browsers (`ChatGPT-User` and similar) do not run `dashboard.js` and read
only the first HTML, so the pull page's first HTML must hold the pull
request (`repo_ssr.py`, `repo_views.py`):

- A short-address pull page (`/<name>/pull/<n>`) is drawn like a long one
  (`inject_view`, links under `/<name>`). When the bundle is not ready in
  time, the page keeps the header and tab strip and starts the bundle read.
- People's pages wait only `VIEW_BUDGET_SECONDS` (0.04 s) for the pull's read
  bundle. A request whose user agent names an AI fetcher (`READER_AGENTS`:
  `ChatGPT-User`, `OAI-SearchBot`, `Claude-User`, `Perplexity-User` and
  similar) waits up to `READER_BUDGET_SECONDS` (5 s), and its Conversation
  page also carries the Files changed diff under a heading. The bundle build
  starts either way; only the wait differs.
- `GET /<owner>/<repo>/pull/<n>.diff` (or `.patch`, or `/pulls/`), and
  `/<name>/pull/<n>.diff` on short addresses, answers the patch as
  `text/plain` (`nosniff`, `no-cache`) from the same bundle; a pull with no
  patch (closed or merged) answers 404. A short name is resolved to its owner
  from the page's `forkmesh-repo-owner` meta, then remembered. A repository
  the gateway does not serve publicly falls through to the App, so the route
  reveals nothing new.
- The first view's pull header has a **Raw diff** link, and a diff cut at
  `MAX_DIFF_FILES` / `MAX_DIFF_LINES` points to it.
- `GET .../pull/<n>.txt` (long and short addresses) answers the whole pull as
  `text/plain`: title, author, branches, state, description, every comment
  and review (edits and deletions applied by
  `repo_views.folded_conversation`, line comments with `path:line`), then the
  diff. It is the `{textUrl}` of the review prompt
  ([pull-review-prompt.md](pull-review-prompt.md)). A closed or merged pull
  answers without a diff; a non-public repository falls through like `.diff`.

Tests: `app/tests/host/test_repo_views.py`.

## Repository share card

The repository page's `og:image` points at `repo_card_handler`
(`app/src/entry.py`): an unfurl shows the repository as a terminal session, a
rounded window with a title bar (three traffic-light dots and
`host - owner/repo`) and a short shell session (`forkmesh repo show <path>`),
the description (at most three lines), the non-zero counts (a single mirror,
the repository itself, is left out), and `git clone https://<host>/<path>.git`
with a block cursor. The repository's mark sits neofetch-style top right,
over a dark wash of the instance accent (`_repo_card_accent`, lightened
until it reads on near-black) and faint scanlines.

- **Visibility before cache.** The signed catalog record is checked first, so
  making a repository private revokes its cached card at once. Organization
  aliases keep their public owner on the card while lookups use the backing
  node.
- **Live counts** (`_repo_card_counts`), read through the router like any
  public read: open issues are the numbered folders of
  `.forkmesh/issues/open`, branches the branch list, contributors the
  committed `contributors.json` (its declared count, else its named rows),
  else commit authors without bots. The catalog's
  `issueCount`/`branchCount` are only the fallback. Counts are remembered per
  repository (lowercased) for five minutes and one card's reads share a
  routing context, so a burst of preview fetches costs one set of reads; the
  caller's `?v=` does not key this memo, so it cannot force fresh reads.
- **Cache.** The PNG is cached under its version and a short token of the
  live counts (`og_card.counts_token`), so a card with other numbers is never
  served for these.
- **Text.** Every string goes through `og_card.terminal_text`: accents
  dropped, typographic quotes and dashes folded to ASCII, anything else `?`.
  Trailing blanks only advance the pen. `CARD_REVISION` (now 4) is bumped
  when the look changes, so preview caches keyed on the `og:image` URL fetch
  the new card.
- **Address and logo.** The card uses the page's address (`/<name>` with
  short addresses) and the owner-uploaded repository logo, else the
  instance's PNG logo (the raster copy `tools/apply_instance_brand.py`
  writes, since crawlers and the renderer cannot draw SVG); only
  forkmesh.com draws the ForkMesh mark. A repository whose only artwork is a
  committed `logo.svg` shows its instance logo. Only 8-bit, non-interlaced
  PNGs of at most 512 x 512 are read: decoding is pure Python on the
  request's event loop, so a larger upload falls back rather than block it;
  the scaled result is remembered per logo.
- **Font.** Geist Mono (SIL OFL 1.1, licence in
  `desktop/resources/fonts/Geist-OFL.txt`). `og_card.py` is pure Python with
  no font rasteriser, so `app/tools/embed_og_mono_font.py` (needs Pillow)
  rasterises every glyph of `og_card.MONO_GLYPHS` into 8-bit coverage
  atlases at authoring time (fixed cells, zlib-compressed, base64-encoded),
  spliced between the generated-block markers in `og_card.py`. Geist Mono
  advances 0.6 em, so the sizes (20, 25 and 35 px; `r25`, `b25` for the pull
  card) are multiples of 5 and every cell is whole pixels. A glyph row is
  blended with three `bytes.translate()` calls. Run the tool after changing
  the sizes or glyph set.

## Pull request share card

A pull page gets its own preview (`app/host/forkmesh_host/pull_card.py`),
drawn by `og_card.render_pull_card` in the same terminal window:

- **Header:** state pill (open, draft, merged, closed), number, commit count,
  age, and the review verdict on the right (approved by N, changes
  requested, review required), computed like the pull list's chip
  (`review_state`).
- **Title** (two lines at most) and who wants to merge which branch where.
- **Diff:** lines added and removed, GitHub-style five squares, files
  changed, comments and how many people wrote them.
- **Code health:** the Files tab's six scores (Merge, Complexity, Quality,
  Security, Network, Practices) as tiles with bars, green 85+, orange 60+,
  red below; a clean Security score is grey, because a pattern scan finding
  nothing is not a security pass. `diff_health.py` is a line-for-line port
  of the dashboard's `diffHealthReport` and `parseDiffFiles`;
  `test_health_scores_match_the_dashboard` runs both under Node.
- **Reviews:** each reviewer's latest review (blocking first, the author's
  own never) with avatar and the start of the text, then ForkBot's review,
  summed up by its Summary section. Rows that do not fit are left off.
- **People:** avatars of everyone involved, ten at most with "+N", and the
  total. Avatars come from `/api/accounts/<name>/avatar?size=128` through
  the App, decoded and scaled with Pillow off the event loop, remembered per
  account for ten minutes; a missing picture, bot or git-only name gets a
  coloured disc with its initial. All reads share a 3 s budget.

Wiring:

- `GET /<owner>/<repo>/pull/<n>.png` (and `/<name>/pull/<n>.png`) answers
  the card from the page's bundle (`repo_views.build`), so a repository the
  gateway does not serve publicly falls through to the App. Cards are
  remembered per bundle version for ten minutes; the PNG is
  `public, max-age=300`.
- `inject_view` rewrites a pull page's `og:*`, `twitter:*`, canonical link
  and `<title>`: the image is
  `.../pull/<n>.png?v=<bundle version>.<CARD_REVISION>`, so previews refresh
  when the pull changes, and the description is a one-line summary. Only
  pages that already carry the public repository's share tags are rewritten.
- Link-preview fetchers (`PREVIEW_AGENTS`: Slackbot, Twitterbot,
  facebookexternalhit, Discordbot, Mastodon, Bluesky's Cardyb, WhatsApp,
  Telegram, LinkedIn and similar) wait up to `READER_BUDGET_SECONDS` for a
  cold bundle, like AI readers, without the diff in the page.
- The bundle's pull carries `authorKey`, `draft` and `commits` (the
  `From <sha>` lines of `commits.mbox`).
- A merged or closed pull has no patch in the bundle, so the card leaves out
  the diff and code health rows.

Tests: `app/tests/host/test_pull_card.py`, the pull card tests in
`app/tests/test_repo_og_card.py`.

## Optional folder reads, repository logos and SVG images

- **Optional folders.** `.forkmesh/releases` (the About rail's latest release
  and the Releases tab) and `.forkmesh/issues` (the Issues tab) are read with
  `fetchRepoOptionalTree` (`06-repo-content.js`), so a repository without
  them logs no 404s. It answers `null` without a request when the root
  listing has no `.forkmesh` folder, reads the parent listing (`.forkmesh`,
  once per page and catalog version) instead of probing a missing child, and
  remembers a 404 in `sessionStorage` for ten minutes, keyed by repository,
  branch and catalog version (`fmv`), so a push that adds the folder shows
  it. The Issues tab passes `fresh: true`: it ignores the remembered miss and
  checks only the root listing, so a just-filed issue is never hidden. A 503
  is still an error.
- **Repository logos.** Catalog records from git nodes and web-hosted
  repositories carry an empty `fileStructure`, so
  `_verified_committed_logo_url` reads the one blob through
  `directory_about.read_blobs` when nothing is listed and offers the URL only
  when the commit holds it, remembered per owner, repository and commit (512
  entries). A mirror that cannot answer keeps the guessed `logo.png`; a
  record listing its root files is trusted without a read.
- **Root `logo.svg`.** The verifying read asks for `logo.png` and `logo.svg`
  together. A raster logo wins; otherwise a `logo.svg` that passes
  `directory_about.logo_type` (starts with `<svg`, no entities, at most
  512 KiB) is offered as `/api/repo/<owner>/<repo>/dir-logo?path=&ref=<commit>`,
  served as `image/svg+xml` with a sandboxing CSP so an `<img>` can draw it
  (the raw route's `attachment` download cannot). A record listing `logo.svg`
  but no raster logo gets the same URL, and `nativeRepositoryLogoDataUrl`
  accepts the `dir-logo` route. Tests:
  `test_an_unlisted_root_logo_svg_is_served_through_the_sanitized_route`,
  `test_repository_cards_accept_the_sanitized_svg_logo_route`.
- **SVG images.** Git nodes serve an `.svg` blob as
  `application/octet-stream` with `attachment` (SVG can carry script), which
  an `<img>` cannot draw. README images and file previews of a repository
  `.svg` put the raw URL in `data-repo-svg-src`; a `MutationObserver`
  fetches it (same origin, at most 1 MiB, must start with `<svg`) and sets a
  `data:image/svg+xml` URL, which an image renders inertly and a browser will
  not open as a top-level page.

## README raw HTML and images

READMEs carry raw HTML: a centred logo (`<p align="center"><img …></p>`), a
contributor wall of `<a href><img src width height></a>` avatars, `<picture>`
with `<source>` variants. Both renderers turn that HTML into the markdown it
stands for and render the markdown, so no README markup reaches the page as
written:

- The dashboard (`flattenMarkdownHtml`, `renderMarkdownInline` in
  `06-repo-content.js`). A linked image becomes a markdown image inside a
  link; the image is parked behind a placeholder before the link is rendered, and
  the link label is rendered by a nested call that shares the placeholder
  list (a separate list rendered the wall as `undefined`).
- The server-rendered first view (`repo_ssr.markdown_html`) flattens raw
  HTML the same way and renders images.

Both keep `width` and `height` (carried as a `#w=WxH` hint), so an image
reserves its box before it loads. Kept: `http(s)` image and link addresses,
same-origin paths, and relative paths, which resolve inside the repository
(images through `/api/repo/<owner>/<repo>/raw`). Dropped: every attribute
other than `src`, `alt`, `width`, `height` and `href`, event handlers,
`style`, `javascript:`/`vbscript:`/`data:` (SVG) addresses, protocol-relative
`//` addresses, `<svg>`, `<iframe>`, `<source srcset>`. External images get
`referrerpolicy="no-referrer"`; instance pages allow `https:` images in their
CSP `img-src`, and there is no image proxy.

## Code addresses with a branch, tag or commit

The app's code addresses are `/<owner>/<repo>/blob/<path>` and
`/<owner>/<repo>/tree/<path>` (or `/<repo>/...` at a short address) for the
default branch, with `?ref=<ref>` for any other ref. The address bar carries
that `?ref=` while a non-default branch is browsed, so a copied link reopens
it; Back and Forward restore the entry's ref.

Links copied from GitHub put the ref in the path
(`/alice/tools/blob/main/README.md`, `/alice/tools/tree/feature/x/src`). A
path after `tree/` or `blob/` with no `?ref=` is read as:

1. If a leading run of segments names a known branch, that is the ref and the
   rest is the path. The longest match wins (`feature/x` beats `feature`). A
   `blob/` address keeps at least one segment for the file. The default
   branch is matched from the catalog; other names need the branch listing
   (`branches`), read once and cached.
2. Otherwise a path on the default branch, so `/blob/README.md` and
   `/tree/docs` behave as always.
3. If that path does not exist and the first segment resolves as a ref on a
   mirror (a tag such as `v1.0`, a full or short commit SHA, a branch the
   listing missed), the rest is read at that ref.

So a folder named like a branch (`main/`) loses to the branch, as on GitHub;
a folder named like a tag or commit wins, because those are tried only when
the path is missing. Once resolved, the address is replaced
(`history.replaceState`) with `/blob/<path>` for the default branch or
`/blob/<path>?ref=<ref>` otherwise.

On a node that reads the repository locally (repo_fastpath), the host
answers a plain GET of a GitHub-style address with a `308` to the canonical
address before the page loads (`app/host/forkmesh_host/repo_code_ref.py`). It
reads the gateway's branch listing (cached 30 s, waited for at most 150 ms)
and redirects only on a listed branch, leaving tags and commits to the page.
A short address is redirected once the App's page names its owner. The
redirect is `no-store` because branches come and go. A path naming no
branch, or an address with a query, is served as before. A failed listing (a
private or missing repository) is cached for the same 30 s. An address with
a `.` or `..` segment (decoded, so `%2E%2E` and `a%2F..` count) is never
redirected, so `Location` never carries a dot segment.

Known cost: the app writes default-branch addresses as `/tree/<path>`, so on
a repository with a branch named like that path's first folder (a `docs`
branch and a `docs/` folder) such a link opens the branch. A link that must
survive this can carry `?ref=`.

There is no `raw/<ref>/<path>` page route; raw bytes come from
`/api/repo/<owner>/<repo>/raw?path=&ref=`.
