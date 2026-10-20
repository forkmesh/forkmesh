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

## Code page About rail

The About rail beside the Code tab is a details list, not a stack of cards
(October 2026 restyle, the "details list" variant the user picked from three).
Under the description and the Readme / Activity / clean-URL links, one row per
fact shows a muted label on the left and the value on the right: Website,
Release, Stars, Watchers, Forks, Mirrors, Files and Size. Stars toggles a star,
Watchers and Forks open their popovers to the right, Mirrors opens the Mirrors
tab, Release opens Releases, and Size opens the Size map tab beside a tiny
sunburst. Rows stay hidden until their data loads.

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
- **Discussions** have a search box (`is:open category:…`), Sort by (latest
  activity, date created, oldest), Filter (open, closed, all) and New
  discussion, with Categories on the left and the list on the right. Each row
  shows the category's emoji, the title, who started it, when, and in which
  category. There are no vote or reply counts because the list reads only
  `discussion.md`. New discussion explains that discussions are created from
  the desktop app, since they are signed by the author's node.

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
