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
  the picker's selected check is blue. The file finder is a modal combobox:
  focus stays between its search and close controls, Arrow keys update its
  active result, Escape returns focus to its opener, and result rows are not
  individual Tab stops.
- The branch merge dialog uses `fm-dialog` inside the native `<dialog>`; the
  `fm-dialog` class sits on the inner form because its `display: flex` would
  otherwise override the closed dialog's `display: none`.
- The issue and pull-request metadata pickers are non-modal popovers: opening
  one moves focus to its first field, and closing it with Escape or Close
  returns focus to the settings button that opened it.
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
- Ctrl/Cmd+Enter in a form field submits through the form's primary button,
  found as `.bg-cta, .fm-btn-cta` (`08-repo-detail-network.js`), so forms
  whose green button is now an `fm-btn-cta` keep the shortcut.

## Repository title row

In October 2026 the user asked for GitHub's repository header: Fork and Star
belong next to the name, where people look for them. In the About rail they
read as stats, and the Fork button was hidden behind the Forks count, so
web-only visitors never found it. Under the tab bar and above the content grid,
`renderRepoTitleRow` in `08-repo-detail-network.js` draws one row with a bottom
border, on every tab:

- **Left:** the repository logo (`nativeRepositoryLogoMarkup`) or the owner's
  initial, the name in bold, and a Public/Private `fm-label`. Under the name is
  the provenance line (`repoProvenanceHtml(repo, { forkButton: false })`):
  "forked from owner/repo" as an accent link, or a mirror's "Mirror of" line
  and read-only notice. The line used to sit above the file table. The mirror
  notice no longer draws its own Fork button, because the row already has one.
- **Fork N ▾:** a neutral `fm-btn-sm` split button.
  - The main part (`data-repo-fork-primary`) opens your own fork when the
    forks list or a create response names one. Otherwise it opens the menu and
    creates the fork right away.
  - ▾ (`data-repo-action-fork`) opens the forks menu: the list, forks on other
    instances, and the page's one green button, "Create fork".
  - Creating a fork calls `POST /api/repo/<o>/<r>/forks`
    (`createRepoForkFromWebsite` in `06-repo-content.js`). On success it
    refreshes the list and shows "Open fork". When the response carries `push`,
    it shows the remote commands, username, password and expiry once
    (`repoForkPushHtml`). `exists: true`, or a response that is neither
    `created` nor `queued`, means you already had a fork.
- **Star N:** "Star N" or "Starred N", with the yellow star. `fm-btn` colours
  every icon grey, so `setRepoStarButtonState` also sets the colour inline.
  Stars have no list (they are blind-indexed), so the button has no ▾.

`renderRepoDetail` draws the Fork button in the Code toolbar too, so it
declares its own `websiteForkAvailable`. Until 2026-10-21 only
`renderRepoTitleRow` declared it, and every repository page stopped with a
ReferenceError before its tabs loaded.

Watch stays in the About rail. The forks menu is anchored to the actions
container, which `ml-auto` keeps at the right edge even when it wraps under
the name on a phone, so the menu always opens on screen. The server-rendered
first view (`_title_row` in `app/host/forkmesh_host/repo_ssr.py`) draws the
same row with placeholder counts, so the page does not shift when the
dashboard takes over. Tests: `app/browser_tests/tests/dashboard-repo-forks.spec.js`,
`app/tests/test_dashboard_repo_mirror_label.py`,
`app/tests/test_dashboard_landing_migration.py` and
`app/tests/host/test_repo_ssr.py`.

## Code page About rail

The About rail beside the Code tab is a details list, not a stack of cards
(October 2026 restyle, the "details list" variant the user picked from three).
Under the description and the Readme / Activity / clean-URL links, one row per
fact shows a muted label on the left and the value on the right: Website,
Release, Watchers, Mirrors, Files and Size. Stars and Forks moved to the
title row above (see the section above). Watchers opens its popover to the
right, Mirrors opens the Mirrors tab, Release opens Releases, and Size opens
the Size map tab beside a tiny sunburst. Rows stay hidden until their data
loads.

Every `http(s)://` address in the description is a link (accent colour, new
tab, `nofollow ugc`), found and trimmed the way the README renderer finds bare
URLs (`MD_BARE_URL`, `mdTrimAutolink`, `mdLinkHtml`); the rest of the text
stays escaped. A read-only mirror without its own description links the
upstream path in "Read-only mirror of …" to the upstream URL. Long addresses
wrap instead of widening the rail. The first render and a saved edit both go
through `repoAboutDescriptionHtml` in `06-repo-content.js`; tests in
`app/tests/test_dashboard_repo_mirror_label.py`.

The rail has no banner or logo header: the user asked for the fediverse banner,
repository logo, handle and follower avatars to be removed from it. The
fediverse handle and watcher list are still in the Watchers popover.

Below the list, Languages, Contributors and Direct access are plain sections
with sentence-case headings. Contributor avatars are neutral initials (the
name and commit count are in the tooltip), not coloured gradients, with the
contributor's picture drawn over them when one is known:

- `avatarUrl` from the gateway's `stats`: the repository's own
  `.forkmesh/info.json` contributor list, else a GitHub no-reply commit email
  (`<id>+<login>@users.noreply.github.com` gives
  `avatars.githubusercontent.com/u/<id>`, `<login>@users.noreply.github.com`
  gives `github.com/<login>.png`; `[bot]` logins without an id get none).
  Only plain `https:` addresses are used, loaded lazily without a referrer; a
  picture that fails to load is removed and the initials show.
- otherwise, a contributor name that is a valid account name is looked up on
  the instance (`/api/accounts/<name>`, the same cached lookup issue actors
  use) and its avatar replaces the initials when the account exists and is
  public. Names with spaces keep initials.

**Contributor roster.** A repository can ship its own list at
`.forkmesh/contributors/contributors.json`, with one avatar per contributor at
`.forkmesh/contributors/<slug>.png`:

```json
{"about": "How the list was made.", "count": 3,
 "contributors": [{"slug": "alice", "name": "Alice", "login": "alice",
   "profile": "https://example.com/@alice", "commits": 120,
   "first": "2024-01-02", "last": "2026-10-01", "projects": ["acme-api"]}]}
```

When the file is there, the Contributors section shows `count` (never below
the number of valid rows) and the top 8 rows by `commits`, each picture read
through the raw route at the selected branch (`repoRawUrl`, as README images are). The
tooltip is the name and commit count; an avatar links to `profile` only when it
is a plain `https:` URL; a missing picture keeps the initials; "+N" opens the
roster file. Other fields are ignored. The file is read with the optional
`.forkmesh` folder machinery below (no request unless `.forkmesh` has a
`contributors` folder; a miss is remembered), at most 2 MiB and 10,000 rows;
a slug must match `[a-z0-9][a-z0-9._-]*`. Without a valid roster the section
uses the `stats` authors as before, minus bots (`… [bot]`, `GitHub Action(s)`,
`… Bot` / `…-bot`, `<slug>-actions`, `<platform> Actions` /
`actions@forkmesh.local` and `ForkMesh Deploy`), which are dropped from both the avatars and the
count. Code: `loadRepoContributorRoster`, `renderRepoAboutRoster`,
`repoContributorIsBot` in `06-repo-content.js`; tests in
`app/tests/test_repo_contributor_roster.py`.

Commit emails are not sent to the browser. Direct
access lists online mirrors as plain rows with no green: the node name, its
version or, for the node serving the page, the response time, and an
open-direct icon. The repository-logo suggestion form and the owner's
fediverse posts sit at the bottom as collapsibles.

## Issues, pull requests and discussions

These three tabs follow GitHub's layouts (October 2026, from the user's
GitHub reference screenshots).

The issue list's label filter lives in the address bar (`?label=…`), so a
filtered list can be linked and survives a refresh. Only the list route is
rewritten; an open issue keeps its own address. A label's swatch is its
colour dot, except Hacktoberfest, which gets a small pumpkin that wiggles a
few times when the list first paints (and stays still for viewers who ask for
reduced motion).

- **Issues and Pull requests lists** share one layout
  (`data-repo-collection-layout`). On wide screens the views sidebar sits
  flush against the left edge of the window with a border down its right
  side, and the list is centred beside it at up to 80rem. "Collapse sidebar"
  is pinned to the bottom of the sidebar and is shared by both tabs.
- **Pull request views** are Pull requests, Authored by me and Mentioned, then
  a Branches link and the base branches of the open pulls. Pulls carry no
  labels, milestones, assignees or review requests in `pull.md`, so the list
  offers only filters it can apply: Author, Base (when there is more than one)
  and Sort. The search box carries them as qualifiers: `is:pr state:open
  author:… base:… mentions:@me sort:…`. Row icons show the state: open green,
  merged primary blue, closed red (purple is not a ForkMesh colour).
- **Pull request page**: no Back button, as on GitHub; the Pull requests tab
  returns to the list (the same holds for Issues and Discussions). The header
  is the title and number, the state pill, "wants to merge N commits into
  `owner/repo:base` from `owner/repo:head`" with a copy button, and boxed
  section tabs with the diffstat on the right. The description is an
  avatar-left comment card; commits and bodiless reviews are lines on a
  timeline rail, and reviews with text are comment cards. The sidebar lists
  only sections with real data behind them.
- **People in timelines** (issue and pull timelines, comment cards, the
  opening card, "set assignees"): every name is drawn by
  `issuePersonNameHtml` and every face by `issueActorAvatarHtml`
  (`02-helpers.js`). The picture is, in order, the forge account's avatar
  (`/api/accounts/<name>`, one cached lookup per name), the repository
  roster's picture for a row whose `login`, `github` or `gitlab` matches the
  name case-insensitively (`avatar` when it points into
  `.forkmesh/contributors/`, else `<slug>.png`), then the initials. A name
  links to `/@<name>` when the forge account exists, else to the roster
  `profile` (plain `https:` only, new tab), else not at all. Bots (`…[bot]`
  and the names `repoContributorIsBot` already treats as bots, such as
  `<org>-actions`) get the lucide `bot` mark and no link. Faces have fixed
  sizes and pictures are laid over the initials with explicit width and
  height and `loading="lazy"`, so nothing moves when they arrive. Comment
  cards keep the 40 px avatar column on wide screens and show a 24 px avatar
  beside the name on phones, where the column is hidden. The roster is read
  once per repository and branch (`repoContributorRosterCached`), shared with
  the About rail. Tests: `app/tests/test_issue_timeline_people.py`.
- **Code map No wrap** (`layoutPrefs`, `fitCode` in `app/public/assets/codemap/codemap.js`):
  by default a file's lines flow into newspaper columns, each cut at the
  file's 97th-percentile line length (at most 110 characters). The toolbar's
  "No wrap" toggle lays the file out as one column, one row per line, sized to
  the tile height (no wider than 80 characters of text), and clips each line at
  the tile's edge instead (`code.clip`). The choice is kept in `localStorage`
  (`forkmesh-codemap-wrap`).
- **Code map text vs binary** (`createLoader` in `app/public/assets/codemap/codemap.js`,
  glue `openRepoCodeMap`): a file is drawn as a binary card only when the host
  answered it as base64 (not UTF-8) or its text holds a NUL byte. A path the
  `/blobs` batch left out (`null`: an upstream timeout or the batch byte
  budget) or a batch that failed is retried on its own up to three times, then
  left blank, never carded. Before this, large `.forkmesh/issues` JSON records
  in a batch showed as "binary" at random. The gateway also cuts a blob over
  `MAX_JSON_BLOB` at a UTF-8 character boundary (`_utf8_prefix`), so a cut
  text file stays text.
- **HTML entities in markdown** (`MD_ESCAPED_ENTITY` in `renderMarkdownInline`):
  text is escaped first, so `&rlm;` used to show as the literal "&rlm;" (the
  example/forum README marks its Persian and Arabic headings with `&rlm;` and
  `&zwnj;`). After code spans are parked, a well-formed named or numeric
  entity is restored, as GitHub renders it; an entity only ever yields text,
  `&lt;script&gt;` stays visible text, and code spans keep the literal.
- **@mentions in markdown** (`MD_MENTION` in `renderMarkdownInline`): an
  `@login` (letters, digits and single inner hyphens, at most 39) becomes a
  link to its profile, resolved like a timeline name; with no forge account
  and no roster profile it goes back to plain text. It is matched after code
  spans, links, images and bare URLs are parked, so nothing is linked twice,
  and never when the `@` follows a letter, digit, `.`, `/`, `+` or `-`
  (emails, URL paths, `@scope/package`), or when the login runs on into
  `.x`, `/x`, `_x`, `@` or `[bot]`. Task markdown opts out
  (`mentions: false`): tasks highlight teammates themselves. Tests in
  `app/tests/test_repo_readme_markdown.py`.
- **Mention suggestions** in every record editor (the Add a comment box and
  its @ button, pull reviews, discussion replies, the issue description
  editor, inline pull comments, and the new issue and new pull forms; a
  textarea opts in with `data-mention-input`). Typing `@` plus characters
  opens a list of at most 8 people at the caret: those in this issue or pull
  (author, commenters, assignees, reviewers, people already mentioned, most
  recent first), then the roster by commits; logins and names match by
  prefix first, then anywhere. There is no forge-wide search because
  `/api/accounts/users` returns every account's picture and is too heavy to
  read for this. Tap, Enter or Tab inserts `@login ` (replacing the rest of a
  half-typed login); Escape closes; arrows move. The list is a `listbox`
  driven by `aria-activedescendant`, rows are 44 px tall, and it is placed in
  the visual viewport so it stays above a phone keyboard. Nothing is fetched
  per keystroke: the candidates come from the loaded record and the cached
  roster. Editing a comment opens an inline editor in its card
  (`openIssueCommentEditor`, replacing the browser prompt, which could not
  offer suggestions); a quiet refresh waits while it is open.
- **Development links** (October 2026; the user asked for GitHub's
  Development section on both pages, with a picker and links made from
  "fixes #71" style comments). There is no link event type: a new signed
  event would need every client and the gateway to learn it. Links are read
  from signed text instead, the way issue relationships are:
  - A pull links issue #N when its title or body names `#N`, or when one of
    its comments or commit messages says `close`, `fix` or `resolve` (any
    tense) `#N`. Code spans and fenced blocks don't count.
  - An issue's own description or comments link pull #N with
    `Linked pull request #N`, `fixed by #N`, `resolved in PR #N` and similar
    lines, and unlink it with `Unlinked pull request #N`. The last line about
    a pull wins, so an unlink also hides a link that comes from the pull's
    text. A plain `fixes #N` on an issue is not a link, because issues and
    pulls are numbered separately and `#N` there means an issue.
  - The settings button on Development (any signed-in visitor, since the
    write is an ordinary comment) opens the sidebar picker. On an issue it
    searches the pull list; on a pull it searches the open issues
    (`loadRepoIssueRelationIndex`). Typing a number links one that isn't
    listed. Closing the picker writes one signed issue comment with a
    `Linked/Unlinked pull request #N` line per change. From a pull page that
    comment goes on each issue, because an issue page reads only the pull
    list, not pull comments. For the same reason a web pull comment that says
    `fixes #N` also writes `Linked pull request #P` on issue #N
    (`linkPullCommentIssues`). A comment made only of those lines shows on
    the issue timeline as a "linked pull request #P" rail line.
  - The issue page reads the pull list in the background
    (`loadRepoDevelopmentPulls`, cached for a minute) when the Pull requests
    tab hasn't loaded it. The rules are `pullOwnIssueNumbers`,
    `issueDevelopmentPullStates`, `issueLinkedPullNumbers` and
    `pullLinkedIssueNumbers` in `06-repo-content.js`, ported to
    `development_pull_states` in `repo_ssr_issue.py` for the first view. The
    desktop and phone apps don't show these links yet. Tests:
    `app/tests/test_development_links_frontend.py` and
    `app/browser_tests/tests/dashboard-development-links.spec.js`.
- **Discussions** have a search box (`is:open category:…`), Sort by (latest
  activity, date created, oldest), Filter (open, closed, all) and New
  discussion, with Categories on the left and the list on the right. Each row
  shows the category's emoji, the title, who started it, when, and in which
  category. There are no vote or reply counts because the list reads only
  `discussion.md`. New discussion (and the empty list's Start a discussion)
  opens the web composer; see [web-discussions.md](web-discussions.md).

## Issue page first view

An issue page (`/<owner>/<repo>/issues/<n>`, or `/<name>/issues/<n>` on an
instance with short addresses) used to arrive as the repository header and
an empty band; `dashboard.js` drew a shimmering frame, read the issue and
replaced the frame with it, and everything below the title moved. The node
now draws the issue into the first HTML (`app/host/forkmesh_host/repo_ssr_issue.py`):

- **Same markup as the dashboard.** `render_issue` draws the article
  `renderRepoIssueDetail` (`06-repo-content.js`) draws for a visitor without
  write access, from the issue's signed-event JSON. Its helpers are ports of
  the dashboard's (`escapeHtml`, `formatTimeAgo`, `parseIssueJson`,
  `issueDetailParsed`, `issueLabelHue`, the timeline people helpers, and
  `renderMarkdown` with its inline rules, @mention links included); icons are
  the `<svg>` lucide 0.468's `createIcons` puts in place of
  `<i data-lucide>`. The frame around it is `renderRepoDetail` with the
  Issues tab open on a record. `dashboard.js` keeps that article in place
  when it takes the page over (`takeRepoFirstViewIssue`, and
  `loadRepoRecordDetail` puts it where the skeleton would go), then paints
  its own render of the same markup over it, keeping every server-drawn node
  it would draw the same way (see **Long threads** below), so nothing moves.
  Any change to
  the dashboard's issue markup must be made in both; the parity tests in
  `app/tests/host/test_repo_ssr_issue.py` run the dashboard's own functions
  under node and compare the two renders (add any new helper the issue view
  calls to the test's `FUNCTIONS`/`CONSTANTS`).
- **Long threads.** A forum thread of a hundred comments used to arrive as
  one HTML page that the node sent only once every comment was rendered
  (example/forum#519: 179 KB gzip, received after 468 ms, DOMContentLoaded
  at 852 ms), and the dashboard then redrew the whole thread. The forum's
  own requirements  are that the whole thread is
  readable from the first HTTP response without JavaScript, on links as
  slow as a DNS tunnel, so the first HTML still carries every comment. What
  changed on 2026-10-21 is how it is sent and taken over:
  - **Streamed.** `inject_issue_page` (`repo_ssr.py`) returns an
    `IssuePage`: the page up to and including the header, the opening post
    and the first `TIMELINE_FIRST` (10) timeline items, then the remaining
    items rendered `STREAM_CHUNK` (10) at a time, then the rest of the page
    (the sidebar, the comment box, the shell's scripts). `finish` answers
    with a `ReadableStream` over it, so the ASGI layer sends each part as
    it is ready (chunked transfer): the top no longer waits for the
    Markdown of every comment. `render_issue_parts` (`repo_ssr_issue.py`)
    leaves `REST_MARK` where the later items go and returns their render
    functions (`Renderer.timeline_items`), each chunk rendered with the
    forge-avatar context set again. The whole article is remembered on the
    bundle once the last chunk is sent; a remembered article goes out in
    one piece. A chunk that fails ends the thread early and still closes
    the page; the dashboard draws what is missing. `inject_issue` joins the
    parts for callers that want a string. Caddy must not hold the stream:
    the instance Caddyfile (`install-instance.sh`) sets `flush_interval -1`
    on the App proxy, as `deploy/core/render.py` already did for the
    served stack; Cloudflare passes chunked HTML through as it arrives.
  - **No competing download.** The page's head script fetches the view's
    `page-reads` bundle (`window.__forkmeshView`); on a server-rendered
    issue page it now waits for `DOMContentLoaded`
    (`_view_reads_head(meta, after_page=True)`), so the record does not
    share a slow link with the HTML the reader is waiting for. The
    dashboard still needs the record once, for the comment form,
    reactions and edit controls, and reads it from that bundle
    (`seededRepoRead`).
  - **Taking over without redrawing.** `renderRepoIssueDetail` builds the
    timeline as entries (`issueTimelineEntries`, each with a lazy `html()`)
    and draws only the first `ISSUE_TIMELINE_FIRST` (10, kept equal to
    `TIMELINE_FIRST` by a test) plus, on in-app navigation, a placeholder
    (`<div data-repo-issue-more data-remaining="N">`, two shimmer rows on
    the rail and "Loading N more comments…"). Every issue render goes
    through `paintRepoIssueDetail` (`loadRepoRecordDetail`, the optimistic
    view, the late relations redraw, `renderCurrentWebIssueDetail`): for the
    sticky bar, header, opening post, sidebar, Fediverse thread and each
    timeline item it keeps the node already on screen when its signature
    matches (`issueNodeSignature`: the markup as first drawn, lucide `<svg>`
    turned back into `<i data-lucide>`, whitespace collapsed; recorded when
    a node is drawn, for the server's page by `keepRepoFirstViewIssue`
    before avatars are filled in). Items on screen past the first ten (the
    server's, or an earlier render's) stay where they are, in front of a
    hidden placeholder, and the stream (`queueIssueTimelineRest`,
    `ISSUE_TIMELINE_BATCH` = 15 entries per `requestIdleCallback`, a 16 ms
    timeout without it) renders each entry in turn and keeps the matching
    node (`placeIssueTimelineItems`), inserting only what differs and
    removing what no longer exists. A guest's page therefore stays the very
    nodes the server sent (no picture is decoded again, nothing moves), a
    member only sees the comments that gain edit buttons swapped, and the
    scroll position holds. Without a server page the placeholder shows and
    the items are appended. Lucide draws new icons once, after the last
    batch. A newer render stops the old stream.
  - **Comment links.** Each comment card has `id="issuecomment-<event id>"`.
    A fragment `#issuecomment-<id>` (or GitHub's `#issuecomment-<number>`
    for a mirrored `forge-github-comment-<number>`) makes the dashboard draw
    through that comment at once and scroll to it, once per address; on the
    server page the browser finds it by itself.
  - **Off-screen cards.** `shell.html` gives timeline comment cards
    `content-visibility: auto` with `contain-intrinsic-height: auto 180px`
    and `scroll-margin-top: 64px` (clear of the sticky title bar). The
    browser lays out and paints only the cards near the screen, which is
    what lets a 500-comment page paint its top at once; their text stays in
    the DOM, so find in page and anchors reach it, and `auto` remembers a
    card's height once drawn. Paint containment clips a card's own
    overflow, which its `overflow-hidden` box already did, except a 2 px
    avatar hover ring at the card's left edge; the reaction picker opens in
    `document.body`.
  - Comment posting appends after the placeholder, avatars and person links
    are filled in by the existing `MutationObserver`, and edit, delete and
    reaction controls are delegated, so streamed items behave like the rest.
  - Tests in `test_repo_ssr_issue.py`: parity of the whole thread (the
    dashboard's first ten plus its streamed rest against the server page),
    the streamed response's parts and order, the dashboard's placeholder and
    fragment handling, and a node harness with a small fake DOM that runs
    the stream (batches of 15, order, the first nodes kept, server-sent
    nodes reused and a stale one replaced).
- **Viewer-dependent parts.** The comment box, the sub-issue Add button and
  the Development settings button depend on who is looking, so both the guest and the member variants are
  drawn and the page's head script picks one before the first paint
  (`html[data-guest]`, `VIEWER_STYLE`). A member's comment-box avatar is
  drawn blank: the node does not know who is signed in.
- **Escaping.** Everything from the issue is escaped, the state label
  included: the issue file's `status` is author-controlled and not checked
  against a signature here. The Markdown port escapes before it formats.
- **Data.** `repo_views` builds an issue bundle (the whole record and the
  issue index's `related` answer, for mentions and sub-issues; the open records on a gateway
  without the index, see
  [issue-scale-limits.md](issue-scale-limits.md#the-issue-index)) from the
  local gateway,
  remembered per content; the rendered article is kept on the bundle for a
  minute because its relative times move. The page waits at most
  `ISSUE_BUDGET_SECONDS` (0.3 s) for the issue and the repository's first
  view, which are usually remembered; otherwise it is served without them.
  Without a remembered `/about`, a feature tab's page still draws the tabs a
  repository has by default.
- **View on GitHub.** When the issue index knows the provider issue an issue
  was mirrored from (`sourceUrl`, see
  [issue-scale-limits.md](issue-scale-limits.md#the-issue-index)), the title
  actions start with a link to it (`renderIssueSourceLink` /
  `source_link_html`): "View on GitHub" for github.com, "View on <host>"
  otherwise, the text hidden below `sm` so the row stays one line on a phone.
  It opens in a new tab with `rel="noopener noreferrer nofollow"`; both
  renderers accept only an `https` address of plain path characters. It is
  a link the reader chooses to follow: nothing on the page loads from the
  provider. The dashboard learns the address with the issue's relations
  (`loadRepoIssueRelations`, kept per issue in `repoIssueSources`), the
  server renderer from the bundle's `related` answer (`index_source_url`).
- **Order, as on GitHub (`forum-list` plugin).** With the instance plugin
  `forum-list` on (an `OPT_IN` plugin: off on instances and on forkmesh.com
  until chosen), the issue list's default sort is Newest (`newest`: created
  time descending, ties by number descending), GitHub's
  `sort=created&direction=desc`; without it the default stays "Recently
  updated" (`issueDefaultSort`). Both stay in the Sort menu, and "Recently
  updated" is what the Recent view picks. The default is not written into the search
  box (`sort:` appears only for another choice). `issueSortItems` falls back
  to Newest for an unknown sort, and the server-rendered list orders its rows
  the same way (`_created_ms`, seconds read as milliseconds like `stamp`).
  A thread shows its comments and events oldest first by their `ts`
  (`renderIssueTimeline` / `Renderer.timeline`, both stable sorts), whatever
  order the record stores them in; mirrored comments carry the provider's
  creation time.
- **Instance switches.** With the `issue-ai-prompt` or `fediverse-ui` plugin
  off ([INSTANCES.md](../INSTANCES.md#plugins)) neither renderer draws the
  prompt box or the Fediverse thread; the server renderer reads
  `plugins_off()` and keys its kept article by it.
- **Pictures.** A paragraph that is one picture of unknown size (no width or
  height, `mdLoneImage` / `lone_image`) is drawn in a fixed 16:9 frame,
  `img[data-md-figure]` in `shell.html`, at most 24rem high, with the picture
  scaled down inside it, so the conversation below does not move when it
  loads.

## Repository share card

The repository page's `og:image` points at `repo_card_handler`
(`app/src/entry.py`), so a Mastodon, Slack or Twitter unfurl shows the
repository as a terminal session: a rounded window with a title bar (three
traffic-light dots and `host - owner/repo`), and in it a short shell session
(`forkmesh repo show <path>`), the description (at most three lines), the
counts that are not zero (a single mirror, the repository itself, is left
out too), and `git clone https://<host>/<path>.git` with a block cursor. The
repository's mark sits neofetch-style in the body's top-right corner, over a
dark wash of the instance accent (`_repo_card_accent`, lightened until it
reads on the near-black terminal) and faint scanlines.

- **Visibility before cache.** The signed catalog record is checked first, so
  making a repository private revokes its cached card at once rather than
  after the TTL. Organization aliases keep their public owner on the card
  while every lookup uses the backing node.
- **Live counts.** The card counts what the repository page counts, read
  through the router like any public read (`_repo_card_counts`): open issues
  are the numbered folders of `.forkmesh/issues/open`, branches the current
  branch list, contributors the committed `contributors.json` roster (its
  declared count, else its named rows), else the commit authors without bots.
  The catalog's `issueCount`/`branchCount` are what a publisher last reported
  and are only the fallback for a count that could not be read. The counts are
  remembered per repository (owner and name lowercased) for five minutes, and
  the reads of one card share a routing context, so a burst of link-preview
  fetches costs one set of reads; the caller's `?v=` does not key this memo,
  so varying it cannot force fresh reads.
- **Edge cache.** The rendered PNG is cached under its version and a short
  token of the live counts (`og_card.counts_token`), so a card drawn with
  other numbers is never served for these.
- **Text.** Every string goes through `og_card.terminal_text`: accents are
  dropped, typographic quotes and dashes fold to ASCII, anything else becomes
  `?`. Trailing blanks only advance the pen, so they never paint over what lies
  past the text. `CARD_REVISION` is bumped when the card's look changes, so
  link-preview caches that key on the `og:image` URL fetch the new card.
- **Address and logo.** The card names the repository by the address its
  page has (`/<name>` on an instance with short addresses, not
  `/<owner>/<name>`) and carries the owner-uploaded repository logo, else
  the instance's PNG logo (the raster copy `tools/apply_instance_brand.py`
  writes, since crawlers and the card renderer cannot draw SVG); only
  forkmesh.com itself draws the ForkMesh mark. A repository whose only
  artwork is a committed `logo.svg` shows its instance's logo. Only 8-bit,
  non-interlaced PNGs of at most 512 x 512 pixels are read: decoding is pure
  Python on the request's event loop, so a larger upload falls back rather
  than block it for seconds, and the scaled result is remembered per logo.
  `CARD_REVISION` went to 4 with this change.
- **Font.** Text is Geist Mono (SIL OFL 1.1, licence in
  `desktop/resources/fonts/Geist-OFL.txt`). The Pyodide worker cannot
  rasterise a TrueType font, so `app/tools/embed_og_mono_font.py` (needs
  Pillow) rasterises every glyph of `og_card.MONO_GLYPHS` into 8-bit coverage
  atlases at authoring time: one fixed cell after another, zlib-compressed and
  base64-encoded, spliced between the generated-block markers in
  `og_card.py`. Geist Mono advances 0.6 em, so the sizes (20 and 35 px) are
  multiples of 5 and every cell is a whole number of pixels wide. A glyph row
  is blended over the flat background with three `bytes.translate()` calls, so
  a whole card is still a few thousand row-slice writes. Run the tool after
  changing the sizes or the glyph set.

## Optional folder reads, repository logos and SVG images

Seen on an instance in October 2026: every repository page logged a 404 for
`tree?path=.forkmesh/releases`, the repository list logged a 404 for a
`logo.png` the repository never had, and a README `<img src="logo.svg">`
stayed blank.

- **Optional folders.** `.forkmesh/releases` (the About rail's latest release
  and the Releases tab) and `.forkmesh/issues` (the Issues tab) are read with
  `fetchRepoOptionalTree` (`06-repo-content.js`). It answers `null` without a
  request when the root listing on screen has no `.forkmesh` folder, reads the
  parent listing (`.forkmesh`, once per page and catalog version) instead of
  probing a child that is not there, and remembers a 404 in `sessionStorage`
  for ten minutes, keyed by repository, branch and catalog version
  (`fmv`), so a push that adds the folder shows it. The Issues tab passes
  `fresh: true`: it ignores the remembered miss and checks only the root
  listing, so a just-filed issue is never hidden. A 503 is still an error.
- **Repository logos.** Catalog records from git nodes and web-hosted
  repositories carry an empty `fileStructure`, so the logo endpoint used to
  guess `logo.png` at the record's commit. `_verified_committed_logo_url`
  now reads that one blob through `directory_about.read_blobs` when nothing
  is listed and offers the URL only when the commit holds it; the answer is
  remembered per owner, repository and commit (512 entries). A mirror that
  cannot answer keeps the old guess. A record that lists its root files is
  trusted without a read.
- **Root `logo.svg`.** The card logo looked only for raster names, so a
  repository whose only root logo is `logo.svg` kept the generic book icon.
  The verifying read now asks for `logo.png` and `logo.svg` together. A
  raster logo still wins; otherwise a `logo.svg` that passes
  `directory_about.logo_type` (starts with `<svg`, no entities, at most
  512 KiB) is offered as `/api/repo/<owner>/<repo>/dir-logo?path=&ref=<commit>`.
  That route serves it as `image/svg+xml` with a sandboxing CSP, so an
  `<img>` can draw it, where the raw route's `attachment` download cannot. A
  record that lists `logo.svg` but no raster logo gets the same URL, and the
  card's URL check (`nativeRepositoryLogoDataUrl`) accepts the `dir-logo`
  route. Tests: `test_an_unlisted_root_logo_svg_is_served_through_the_sanitized_route`,
  `test_repository_cards_accept_the_sanitized_svg_logo_route`.
- **SVG images.** Git nodes serve an `.svg` blob as
  `application/octet-stream` with `attachment` (SVG can carry script), which
  an `<img>` cannot draw. README images and file previews that name a
  repository `.svg` put the raw URL in `data-repo-svg-src`; a
  `MutationObserver` fetches it (same origin, at most 1 MiB, must start with
  `<svg`) and sets a `data:image/svg+xml` URL, which an image renders inertly
  and a browser will not open as a top-level page.

## README raw HTML and images

READMEs carry raw HTML: a centred logo (`<p align="center"><img …></p>`), a
contributor wall of `<a href><img src width height></a>` avatars, `<picture>`
with `<source>` variants. Both renderers turn that HTML into the markdown it
stands for and render the markdown, so no README markup reaches the page as
written:

- The dashboard (`flattenMarkdownHtml`, `renderMarkdownInline` in
  `06-repo-content.js`). A linked image becomes `[![alt](src)](href)`; the
  image is parked behind a placeholder before the link is rendered, and the
  link label is rendered by a nested call. That call used to start its own
  placeholder list, so it looked the image up in an empty list and the wall
  rendered as the text `undefined`. The placeholder list is now shared with
  the nested call.
- The server-rendered first view (`repo_ssr.markdown_html`), which used to
  drop raw HTML and print images as their alt text, now flattens raw HTML the
  same way and renders images.

Both keep `width` and `height` (carried as a `#w=WxH` hint), so an image
reserves its box before it loads. Kept: `http(s)` image and link addresses,
same-origin paths, and relative paths, which resolve inside the repository
(images through `/api/repo/<owner>/<repo>/raw`). Dropped: every attribute other
than `src`, `alt`, `width`, `height` and `href`, event handlers, `style`,
`javascript:`/`vbscript:`/`data:` (SVG) addresses, protocol-relative `//`
addresses, `<svg>`, `<iframe>`, `<source srcset>`. External images get
`referrerpolicy="no-referrer"`; instance pages already allow `https:` images in
their CSP `img-src`, and there is no image proxy.

## Code addresses with a branch, tag or commit

The app's own code addresses are `/<owner>/<repo>/blob/<path>` and
`/<owner>/<repo>/tree/<path>` (or `/<repo>/...` at a short address) for the
default branch, with `?ref=<ref>` added for any other ref. The address bar
now carries that `?ref=` while a non-default branch is browsed, so a copied
link reopens the same branch; Back and Forward restore the ref of the entry.

Links copied from GitHub put the ref in the path instead:
`/alice/tools/blob/main/README.md`, `/alice/tools/tree/feature/x/src`. They
used to read `main/README.md` from the default branch and show "File
unavailable". They now open the right file and settle on the app's address.

How a path after `tree/` or `blob/` is read when the address has no `?ref=`:

1. If a leading run of segments names a known branch, that is the ref and the
   rest is the path. The longest match wins, so `feature/x` beats `feature`.
   A `blob/` address keeps at least one segment for the file. The default
   branch is matched from the catalog without a listing; other names need
   the branch listing (`branches`), read once and cached.
2. Otherwise it is a path on the default branch, exactly as before, so
   `/blob/README.md` and `/tree/docs` behave as they always have.
3. If that path does not exist and the first segment resolves as a ref on a
   mirror (a tag such as `v1.0`, a full or short commit SHA, a branch the
   listing missed), the rest is read at that ref.

So a real folder named like a branch (`main/`) loses to the branch, just as
it does on GitHub. A folder named like a tag or commit wins, because tags and
commits are not listed and are only tried when the path is missing.

Once resolved, the address is replaced (`history.replaceState`, no new
history entry) with `/blob/<path>` for the default branch or
`/blob/<path>?ref=<ref>` for any other ref.

On a node that reads the repository locally (repo_fastpath), the host answers
a plain GET of a GitHub-style address with a `308` to the same canonical
address before the page loads (`app/host/forkmesh_host/repo_code_ref.py`). It
reads the gateway's branch listing (cached 30 s, waited for at most 150 ms)
and redirects only on a listed branch, so tags and commits are left to the
page. A short address is redirected once the Worker's page names its owner.
The redirect is `no-store` because branches come and go. A path that names no
branch, or an address that already has a query, is served as before.
A failed listing (a private or missing repository) is cached for the same
30 s, so repeated GitHub-style hits on it do not each wait on the gateway.
An address with a `.` or `..` segment (decoded, so `%2E%2E` and `a%2F..`
count) is never redirected, so the `Location` header never carries a dot
segment.

One known cost: the app still writes its own default-branch addresses as
`/tree/<path>`, so on a repository that also has a branch named like that
path's first folder (a `docs` branch and a `docs/` folder), such a link opens
the branch. A shared link that must survive this can carry `?ref=`.

There is no `raw/<ref>/<path>` page route; raw bytes come from
`/api/repo/<owner>/<repo>/raw?path=&ref=`.
