# Feeds, the lite view and the forum home page

All of it is the `read-surfaces` instance plugin (an `OPT_IN` plugin, so it
is off on instances and on forkmesh.com until an admin turns it on under
Plugins, or `FORKMESH_CORE_PLUGINS` names it). While it is off the host
answers none of these paths, repository pages carry no feed or lite links,
and the sidebar's Lite link stays hidden.

Some readers of a public repository's issues never run the dashboard: people
behind DNS tunnels or other slow, filtered links, and people who follow a
forum by email through a feed-to-mail tool such as rss2email. A forum
mirrored from GitHub issues (`example/forum` below) is the case this was
built for. Three read surfaces serve them without
JavaScript, from data the node already has:

| Address | What it is |
| --- | --- |
| `/<o>/<r>/issues.atom` | New threads, newest first, the opening post as content |
| `/<o>/<r>/activity.atom` | New threads and new comments, newest first |
| `/<o>/<r>/issues/<n>.atom` | One thread: the opening post and its comments |
| `/<o>/<r>/labels/<label>.atom` | New threads with one label (`issues.atom?label=` is the same feed) |
| `/<o>/<r>/lite` | Thread list, 50 a page, `?page=`, `?label=`, `?sort=active` |
| `/<o>/<r>/lite/issues/<n>` | The whole thread on one page |
| `/lite` | The instance's public repositories, the home repository first |
| `/` | The forum home page, on an instance with a home repository |
| `/<o>/<r>/mirror` | How to help: run a mirror, join the federation, register it |

The user-facing guide is [www/docs/feeds-and-email.md](../../www/docs/feeds-and-email.md).

## Where they are answered

`app/host/forkmesh_host/read_surfaces.py`, called from `HostASGI._http`
right after the `/api/repo/` fast path and before static assets and the
Worker. `read_surfaces.target(scope)` recognises the paths (owners in
`repo_ssr.RESERVED_OWNERS` and `*.git` names are never matched, so
`/api/...`, `/dashboard/...` and Git URLs are untouched). A repository the
node's gateway does not serve publicly, a missing issue index, or no home
repository for `/` makes `handle` return `False` and the request goes on to
the Worker exactly as before, so on forkmesh.com and on any node without the
fast path nothing changes. A `.atom` path is never read as a repository file
path because it never reaches the repository routes.

Only the long `/<owner>/<repo>` form is routed. A short repository address
(`/<name>`) has no lite view or feed of its own; the dashboard's links
(`09-read-surfaces.js`) always build the long form.

## Data: the issue index and the records

Nothing new is stored. Every answer is built from three gateway reads, all
pinned to one commit:

1. `tree?path=` without a ref gives the default branch's commit, and doubles
   as the public check (`Source.head`, remembered for `HEAD_FRESH_SECONDS`).
2. `issue-index?ref=<commit>`: titles, authors, labels, comment counts,
   created and updated times and the open/closed folder of every issue. A
   commit-pinned read is immutable, so the fast path's `ImmutableCache`
   keeps it (with its parsed JSON) after the first request.
3. `blobs?path=...&ref=<commit>` in batches of `BLOB_BATCH` for the records a
   page needs (`.forkmesh/issues/{open,closed}/<n>/issue-<n>.json`), plus
   `.forkmesh/attachments/issues/<n>.json` when the attachment mapping exists.

Records are folded with `repo_ssr_issue.parse_issue` (the issue page's own
parser) and `read_surfaces.posts`: the opening post with its latest edit,
then each comment that is not deleted, with its latest edit. Deleted issues
are left out everywhere.

Each rendered answer is kept in a process cache (`_cache`, bounded to
`CACHE_BYTES`) under a key holding the commit, the route, its parameters and
the origin. The ETag is a hash of that key, so a reader polling a feed gets
`304 Not Modified` without any rendering, and a push (a new commit) changes
every ETag. Responses say `Cache-Control: public, max-age=60` and carry
`Last-Modified` from the newest entry. The home page's key also holds the
instance document and a 60 second bucket, because the mirror line on it does
not follow the commit.

## Markdown

Post bodies go through `repo_ssr_issue.render_markdown`, the renderer of the
server-rendered issue page, so mentions, `#123` references, tables, quotes
and code look the same as on the full page. `clean_markup` then:

- drops `class`, `data-*`, `target`, `decoding` and `referrerpolicy`
  attributes (they are the dashboard's Tailwind hooks; without them a
  98-comment thread loses about a third of its bytes);
- keeps an `<img>` only when its source is on the instance (`/api/repo/.../raw`
  for repository files and mirrored attachments, or a `data:` image), adding
  `loading="lazy"`. Any other image, including every GitHub-hosted one
  without a mirrored copy, becomes a text link `[alt]`, so a reader's browser
  or mail client never fetches from GitHub. `<video>` becomes a `[video]`
  link.

When `repo_ssr_issue` has the attachment mapping (`attachment_files`, from
the images work), the context gets the issue's mapping and mirrored
attachments point at the instance's copy; until then GitHub attachment
addresses stay text links.

## Atom

Atom 1.0 with the threading extension of RFC 4685
(`xmlns:thr="http://purl.org/syndication/thread/1.0"`):

- Entry ids are `tag:` URIs that never change:
  `tag:<host>,2026:<o>/<r>/issues/<n>` for a thread and
  `tag:<host>,2026:<o>/<r>/issues/<n>/comment/<eventId>` for a comment (the
  record's own event id, so a mirrored GitHub comment keeps its id).
  Feed ids follow the same scheme (`.../issues`, `.../activity`,
  `.../labels/<label>`, `.../issues/<n>/feed`). The year is the fixed
  `TAG_DATE`, not the current one.
- A thread entry has `<thr:total>` with its comment count and a
  `rel="replies"` link to its own feed with `thr:count`. A comment entry has
  `<thr:in-reply-to ref="<thread id>" href="<issue page>" type="text/html">`,
  which is what feed readers and rss2email use to thread replies.
- Authors: a mirrored `github:<login>` is `@login` with
  `<uri>https://github.com/<login></uri>`; an account on the instance is its
  name with the profile URL.
- `<content type="html">` is the cleaned markup, escaped once, with every
  root-relative `href` and `src` made absolute. A body longer than
  `FEED_BODY_CHARS` is cut and ends with a "Continue reading" link.
- Size: 50 entries by default, `?limit=` from 1 to `FEED_LIMIT_MAX` (200).
  `activity.atom` reads the `limit` most recently updated threads and keeps
  the newest `limit` posts among them; the thread feed keeps its newest
  `limit` posts.
- A comment entry's alternate link is the comment's anchor on the lite thread
  page (`#c<id>`), which works without JavaScript.

## Lite pages

Plain semantic HTML: one inline `<style>` (`STYLE`, under 2 KB) with system
fonts, a 48em column, light and dark colours from `prefers-color-scheme`
(the `foreground`, `background`, `muted-foreground`, `primary` and `border`
values of `app/public/dashboard/theme.json`), logical properties for quotes,
`dir="auto"` on every title and post body, no script, no external CSS, fonts
or images. The response carries a CSP of `default-src 'none'; img-src 'self'
data:; style-src 'unsafe-inline'` so a stray tag in a post could not load
anything either.

- The list shows title, number, author, date, comment count, `closed`, and up
  to six labels per thread; above it the repository's labels by use, linking
  to the label view. Prev/next links page through 50 threads.
- The thread page shows every post with `@login` and a UTC timestamp, and
  links to the neighbouring thread numbers, the list, the full view and the
  thread's feed. Post anchors are `c` plus the numeric tail of the event id
  (`c3289654410` for `forge-github-comment-3289654410`), `c0` for the opening
  post.

### Weight budget

`app/tests/host/test_read_surfaces.py` holds the budgets: the list page for
50 threads at most 30 KB, the style under 2 KB, and on a thread page the page
chrome (everything but the posts) at most 6 KB, with each post's own wrapper
(article, byline, time) about 100 bytes. Measured on the live example/forum
data (611 issues, commit 8b680799):

| Page | Size | First render | Cached |
| --- | --- | --- | --- |
| `/example/forum/lite` | 18.4 KB | 9 ms | 0.1 ms |
| `/example/forum/lite/issues/519` (98 comments) | 442.8 KB, of which 429.1 KB post bodies; 3.0 KB chrome + 99 x 108 B wrappers | 168 ms | 0.2 ms |
| `/example/forum/lite/issues/637` (25 comments) | 40.0 KB, 35.3 KB bodies | 23 ms | 0.1 ms |
| `/example/forum/issues.atom` (50 threads) | 375.9 KB | 187 ms | 0.2 ms |
| `/example/forum/activity.atom` | 125.7 KB | 70 ms | 0.1 ms |
| `/example/forum/issues/519.atom` | 269.0 KB | 94 ms | 0.2 ms |
| `/example/forum/labels/reading%20group.atom` | 386.3 KB | 160 ms | 0.2 ms |
| `/` (forum home) | 20.1 KB | 14 ms | 0.2 ms |

Times are from a development machine reading the live data over HTTP into
the host code; a 1 vCPU node renders more slowly, once per commit.
For comparison, a Hacker News item page is about 6.4 KB of HTML.

## Forum home page

An admin picks a home repository in the admin console, Platform → Home page
(`instance_console.render_home` / `save_home`, setting row `home`,
`app/src/instance_home.py`). `GET /api/instance` reports it as
`"home": {"repository": "owner/name"}` (or `null`). The host reads that
document through the in-process Worker (`read_surfaces.instance_doc`,
remembered for a minute) when `/` is asked for; with a home repository whose
issues the gateway serves publicly, `/` is the forum page instead of the
redirect to `/dashboard`:

- a header with the instance logo and name, and links to the threads, the
  lite view, the activity feed, the dashboard and sign-in;
- the repository README rendered by the same markdown pass, split at every
  `h1`-`h3` into `<section dir="auto">`, so each language of a multilingual
  README (one with Persian and Arabic sections) takes its own
  direction;
- the 20 most recently active threads, linking to the full issue pages;
- label chips with counts, linking to the lite label view;
- `<section data-forum-mirrors>`, the slot for the mirror work: when
  `GET /api/repo/<o>/<r>/mirror-quota` answers within `QUOTA_BUDGET_SECONDS`
  it holds one line ("3 healthy mirrors of 10 wanted · Mirrors · clone ..."),
  otherwise it stays empty.

The page is server-rendered and needs no JavaScript; every link leads into
the dashboard or the lite pages. With a query string, or without a home
repository, `/` behaves as before.

## Links from the normal pages

- The issue list and issue pages get `<link rel="alternate"
  type="application/atom+xml">` tags for the thread feed (on a thread), the
  new-threads feed and the activity feed, and a `<noscript>` line after
  `<body>` pointing to the lite equivalent and the feed
  (`repo_ssr.read_links`, used by `inject`, `inject_issue`, `inject_view` and
  `inject_view_reads`).
- The issue list ends with "Feed · Lite view", in the server-rendered first
  view (`repo_views._list`) and in the dashboard (`repoIssueFeedLinks` in
  `09-read-surfaces.js`, appended by `renderRepoIssues`).
- The sidebar's bottom row (Security, Privacy, Terms) has a "Lite" link to
  `/lite`; on a repository page a click handler points it at that
  repository's lite list, or at the thread when an issue is open.

Every read-surface page wraps its body in `<!--email_off-->` …
`<!--/email_off-->`: behind Cloudflare, Email Address Obfuscation otherwise
injects an `email-decode.min.js` script into any page with an address-like
string, which broke the "no JavaScript" promise of the lite pages.

## Asking for mirrors

The forum needs independent copies more than anything else, so every read
surface asks for them while a repository is below its mirror goal
([mirror-quota.md](mirror-quota.md)). The texts and markup live in
`app/host/forkmesh_host/mirror_help.py`.

**When.** `mirror_help.help_state(quota, home)` decides: with a mirror quota
answer, only when it has a goal and fewer healthy mirrors than the goal
(`{healthy, target, known: true}`); without one (the API is off, slow or
missing), only for the instance's home repository, and then without a count
(`known: false`), because the message matters more than the number. The
quota is read through the Worker (`read_surfaces.quota_for`, anonymous,
`QUOTA_BUDGET_SECONDS`, or `QUOTA_PAGE_BUDGET_SECONDS` for the how-to-help
page) and remembered for `QUOTA_FRESH_SECONDS`; a failed read keeps the last
good answer. The help state is part of each cache key, so a page changes when
the count does, not only on a push.

**`/<o>/<r>/mirror`.** A lite page (same style, no script, no image, links on
the instance except the installer, forge sync docs, mirror docs and
`https://forkmesh.com/start`): "Help keep this forum reachable", why copies
matter (a clone carries every thread in `.forkmesh/issues`, signed), the
status word in its colour (goal met, behind, urgent: `success`, `attention`,
`destructive`), "N of goal healthy mirrors", the list (name, address as text,
host, status, last check in UTC) and "N more mirrors needed", then four steps
with copy-paste blocks: a git mirror (`git clone --mirror`, a cron line with
`remote update --prune` and `update-server-info`, `caddy file-server`), a
ForkMesh instance (a minimal `settings.json`, the `install-instance.sh`
command, forge sync's mirror mode, the Start your own instance page), a static
read-only copy (generic until the export tool lands), and registering it (Add
a mirror on the repository page, or `POST .../mirror-quota`). "Why we're
asking" quotes the forum's requirement for example/forum
(`mirror_help.REQUIREMENTS`, linking the local #637). The page works for any
public repository, also one without issues; without a quota answer it says
the count is not available and keeps the instructions, with the clone address
built from the request's origin.

**Banners.**

- Forum home: `<aside class="help big" data-forum-help>` right under the
  header, with "This forum needs your help — N of goal mirrors running.", one
  line on why, and one button in `primary` blue (`.go`) to the page.
- Lite list: one `<p class="help">` line under the breadcrumb, an `attention`
  border on the subtle background.
- Issue list: `banner_dashboard` in the first view (`repo_views._list`, from
  the remembered quota) and `repoMirrorHelpBanner` in the dashboard
  (`09-read-surfaces.js`), the same markup (a parity test runs the JS under
  node): one truncated line in `bg-attention/10` with an `attention` dot
  above the list's filter bar. The dashboard version has a dismiss button
  that stores `forkmesh.mirrorHelpDismissed:/<o>/<r>` in `localStorage`; the
  first view and the lite pages always show it. When the dashboard has no
  quota yet, `repoMirrorHelpSlot` loads it and fills the slot.
- Feeds: the feed's `<subtitle>` ("This forum needs your help: N of goal
  mirrors running. Run a mirror or join the federation.") and a
  `<link rel="related" type="text/html">` to the page, both valid Atom feed
  metadata placed before the entries. Thread feeds do not carry it.
