# Page performance

Targets: repository, pull request, issue and discussion pages usable in under
200 ms on a warm load and under 800 ms cold, with immutable-content API reads
under 30 ms at the node.

## Techniques in place

| Technique | Where |
|---|---|
| **Host fast path.** Public repository reads (`tree`, `blob`, `blobs`, `raw`, `history`, `commit`, `compare`, `branches`, `search`, `stats`, `sizes`) are answered from the local Git gateway before the app; non-200 answers fall through. | `app/host/forkmesh_host/repo_fastpath.py` |
| **Gateway caching.** Commit-keyed response cache, single-flight on concurrent misses, a split commit-graph with changed-path Bloom filters written at warmup, and `TCP_NODELAY` on responses. | `tools/mirror_gateway.py` (`FORKMESH_GATEWAY_COMMIT_GRAPH=0` disables the graph) |
| **Early reads.** The page head starts catalog, tree, README, file and history reads right after `api-client.js`; the dashboard takes them over on an exact key match. | `partials/repo-early-reads.html` |
| **Server-rendered first view** of the repository, pulls, pull (conversation, files, commits), issues, issue, discussions and discussion pages, with the JSON the dashboard reuses. | `repo_ssr.py`, `repo_views.py` |
| **View bundles.** `GET /api/repo/<o>/<r>/page-reads?view=pulls\|pull\|issues\|issue\|discussions\|discussion[&number=N]` makes a view's gateway reads in parallel and returns them in one response. Bundles and first views are remembered 5 s, then served stale while one background build refreshes them; a page waits at most 40 ms for a first view. | `repo_views.py` |
| **Immutable diffs.** A `compare` between two commit ids is cached in browser and host memory. | dashboard, host |
| **Session check in parallel.** The page head starts the session read while `dashboard.js` downloads. | dashboard shell |
| **Per-page chunks.** Page-only modules load as content-hashed `/dashboard-page-<name>.js`, retried once on failure. | `app/public/dashboard/js/` |
| **Anonymous `about` cache.** 15 s fresh, then stale-while-refresh up to 5 minutes. | app |
| **One request for a view's reads, even when the first view misses its budget.** A pull, issue or discussion page whose bundle is not ready in 40 ms still carries `window.__forkmeshView` with the unversioned `page-reads` address, so the browser joins the build already running instead of reading the pull ledger, `pull.md` and the conversation one after another. | `repo_ssr.finish`, `inject_view_reads` |
| **Pull detail reads in parallel.** After the ledger commit is known, the pull's folder listing, patch, conversation, commits, agent activity, merge permission, readiness and merge base start together; checks and rules follow readiness. One shared folder listing per ledger commit tells the page which files exist, so a missing `commits.mbox` or `changes.patch` is never asked for (a missing file walked every mirror and then the app, about 1.5 s). Two loaders that need `/branches` share one read. | `loadRepoRecordDetail`, `readRepoPullFolder`, `repoPullFolderLacks` |
| **Issue and discussion detail.** An issue's permission and subscription reads start with its record; a discussion's replies start with its record. An issue the batched read reports missing (`null`) is not probed again; paths the batch did not answer are probed together. | `loadRepoRecordDetail`, `fetchRepoIssueBlob` |
| **Icons drawn once.** Lucide keeps `data-lucide` on the SVG it draws, so each of the dashboard's ~140 `createIcons()` calls redrew every icon on the page (about 130 ms of the repository page's boot). `installIncrementalIcons` replaces it with a pass that draws only `<i data-lucide>` placeholders and SVGs whose name changed. | `02-helpers.js` |
| **Mirror list repaint once per frame.** Every repository read reports the node that served it; the mirror lists repaint once per animation frame instead of once per read. | `renderRepoServedBy` |
| **Latin font subsets.** The dashboard and site load 27 KB Latin subsets of DejaVu Sans Mono (regular and bold) instead of the 147 KB full files; the full file is declared for the remaining ranges and only downloads when a page shows those characters. | `dashboard/shell.html`, `www/public/styles.css`, `www/public/assets/fonts/*-latin.woff2` |
| **Size map rail reads two levels.** The rail's chart draws two rings, so it asks `/sizes?depth=2` (24 KB for forkmesh/forkmesh instead of 131 KB). | `loadRepoAboutSizeMap` |
| **Metadata folders skip code analysis.** Gateway tree reads under `.forkmesh/discussions` and `.forkmesh/releases`, like pulls and issues, skip the whole-repository analysis and per-entry history walks (the releases folder took up to 3.4 s on a cache miss). | `COLLABORATION_TREE_ROOTS` in `tools/mirror_gateway.py` |
| **Genie after load.** The spider starts in idle time after the `load` event rather than at `DOMContentLoaded`, so it no longer competes with the dashboard's first render. | `www/public/genie-spider.js` |
| **Static weight.** Self-hosted icons, small logos, precompressed assets (`.br` when `brotli` is in the deploy venv, plus gzip), immutable caching for hashed files, `no-cache` for pages carrying a rendered view. | build, Caddy |

## The first paint is the final layout

Pages used to paint one thing and then replace it: the repository page's
server-rendered first view was a narrow, differently styled table (blue
`name/` links, no tabs, no About rail) that dashboard.js swapped for the full
interface 300-800 ms later (several seconds on a slow device); the header was
drawn with signed-in controls and blank icon boxes that became "Sign Up / Log
In" and icons after the session check; the instance name beside the logo and
the hiding of switched-off plugin links came from `instance-brand.js` after
the first paint. Layout-shift scores missed most of it because a replaced
block is not a shift. What each page now paints first:

| Piece | Now |
|---|---|
| Repository root (`repo_ssr.render_view`) | The dashboard's own markup and classes: tab strip, toolbar, path bar, mirror line, commit summary, file rows with icons and "N hours ago" dates, README card, About rail with placeholders. Rows and boxes land on the same pixels as the dashboard's (`test_first_view_rows_and_frame_match_the_dashboard_markup`, `test_first_view_uses_dashboard_classes_that_the_stylesheet_has`). Tabs follow the remembered anonymous `about` (Code alone when there is none); `.forkmesh/info.json` (`about`, `website`) is read with the first view. The page header gets the repository name and "public". |
| Dashboard takeover | `applyRepoFirstViewAbout` applies the first view's features, description and website before the dashboard paints its own frame, so its tabs and rail do not change after it has drawn them. |
| Other repository routes | `[data-repo-frame]`: the tab strip's band and the canvas colour instead of "Choose a repository" in the corner; `initRepoPage` keeps it instead of writing "Loading repository…". |
| About rail | Files and Size rows show "–", Languages and Contributors show placeholders of their final size (`data-repo-about-pending`, hidden only if their data never comes). The size map observes its row instead of inserting a marker. |
| Header | A head script marks `html[data-guest]` when there is neither a `forkmesh_session` cookie nor a stored session, and CSS shows the guest header from the start; the dashboard clears it when a session turns up. The always-visible header icons are inline SVG. `:where(i[data-lucide])` is `inline-block`, so every other icon placeholder already has its size. |
| Profile | The contribution period buttons (Last year and the five years before) are in the page with year-wide placeholders filled by an inline script; the summary keeps two lines; the About panel's edit button no longer sets the panel's height (guests do not see it). |

Fonts were not the cause: DejaVu Sans Mono is a Latin subset with
`font-display: swap` used only for counts and hashes, and the theme is set by
the inline head script before the stylesheet.

Measured on a white-label instance, signed out, headless Chromium 1366×900, three
cold loads each; "after" serves this branch's pages, scripts and instance
stylesheet in place of the live ones. CLS is the sum of layout shifts;
"first view" is the share of pixels that differ between the first contentful
frame and the settled page.

| Page | CLS before | CLS after | First view before | First view after |
|---|---|---|---|---|
| short-address monorepo root | 0.0036 | 0 | 14-18 % | 7 % |
| `/dashboard` | 0.0047 | 0.0027 | 1.8 % | 0.9 % |
| mirrored repository root | 0.0291 | 0.0016 | 14-17 % | 7.5 % |
| `/@name` profile | 0.1649 | 0.0077 | 8.5 % | 7.1 % |
| repository Actions tab | 0.0010 | 0.0002 | 12.9 % | 8.3 % |

What still differs after the first paint is data the first view does not
have: counts and stars, the branch count, the release row, the languages and
contributors, the contribution skyline, and on tab routes the panel itself.
Pull request, issue and discussion first views (`repo_views`) still use the
older `.fm-ssr` styling.

## How to measure

Headless Chromium through Playwright, recording each request's start, TTFB,
duration, size, encoding and `Cache-Control`, plus FCP and the moment the
page's data is visible. Cold is a new context, warm a second load, hot a
third. Compare a live run from the operator box with a local single-node stack
built from the release pipeline (precompressed `app/dist`, Caddy with the
generated routes, the Python host, the gateway serving a bare clone) under
network emulation of about 30 ms per request.

## Deploy notes

- The fast path is on wherever `FORKMESH_NODE_ID` is set; point it elsewhere
  with `FORKMESH_HOST_REPO_GATEWAY=http://…` or turn it off with `off`.
- The gateway writes its commit-graph at the first warmup after a restart.
- The first bundle after an app restart is built in the background; that one
  request is served without a rendered view.

## Server load

The host logs `slow request <ms> <method> <path>` above 250 ms (except the
long-polled `/api/runners/claim`). On single-vCPU cores slow lines mostly come
from CPU shared with Git work and from clients sending one request per item.
Rules that follow:

- Prefer one batched read (`blobs` accepts up to 60 paths; view bundles; the
  public account directory) over one request per item.
- Keep heavy history walks on the commit-graph (`git rev-list` with
  timestamps rather than repeated `git log` walks).
- Roll a core only when rqlite reports a stable leader with all voters
  reachable; elections during rolls surface as `503 d1 unavailable`, which
  `/status` counts separately from code faults.

## Open items

- `dashboard.js` is large; most of it is reachable from any page's init, so
  further splitting needs restructuring.
- Home, repositories, network and status pages still render data client-side
  after the dashboard boots.
- `tailwind.css` is not content-hashed.
