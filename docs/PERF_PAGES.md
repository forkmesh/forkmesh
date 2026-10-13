# Page performance (2026-09-24)

Two passes: the repository page (first), then pull requests, issues, discussions and the site-wide costs (second, below).

## Pass 1: the repository page

Target: `https://app.next.forkmesh.com/forkmesh/forkmesh` usable (file list + README painted)
- in under 200 ms on a warm load;
- in under 800 ms cold;
- with immutable-content API reads under 30 ms at the node.

## How it was measured

- Headless Chromium through Playwright (`measure.js` in the session scratchpad):
  - every request with its start, TTFB, duration, size, encoding and `Cache-Control`;
  - FCP, DCL, and the moment the file list, README, file view (`/blob/README.md`) and history (`/commits`) are visible, gated on first paint.
- "Cold" is a new browser context. "Warm" is a second load in the same context. "Hot" is the third load, when V8's code cache applies.
- **Live:** from the operator box, about 50 ms from ewr.
- **Local:** a 1-node stack built from the release pipeline:
  - `app/dist` staged by `build_site_assets.py` and precompressed like `next_deploy.py`;
  - Caddy 2.6 with the generated `app-routes.caddy`, h2 and TLS;
  - the Python host;
  - `tools/mirror_gateway.py` serving a bare clone of forkmesh/forkmesh (53k commits);
  - Chrome's network emulation adding 30 ms per request.
- `/api/repositories` came from a copy of the live catalog, because the local DB is empty.
- The "before" stack used direct mirror reads, as live did.

## Before: where the time went

| # | Cost | Evidence |
|---|---|---|
| 1 | **Cold gateway reads after every push.** The response cache keyed the literal query, so the warmup's ref-less tree was never the web UI's `tree?path=&ref=main`. Every entry ran its own `git log` over full history, with no commit-graph. | Live, right after a push: root tree **5.4 s**, `stats` 4.6 s, `.forkmesh/releases` 5.7 s; tree painted at 6.1 s. Locally: `desktop/src` **64 s**, `app/src` 8.5 s. |
| 2 | **Serial client chain.** HTML → 1.9 MB `dashboard.js` (483 KB gz) → `/api/repositories` → tree → README, each only after the previous one. | Tree request started at 640–720 ms cold; README about 90 ms after the tree. |
| 3 | **~40 ms Nagle stall** on every gateway response over a kept-alive connection. The gateway sent headers and body in separate writes, then waited for a delayed ACK. | A cached tree took 43 ms over keep-alive and 1.4 ms without the stall. Live `tree` 79 ms, README 150 ms. |
| 4 | **Static weight and round trips.** | `/assets/logo.png` 736 KB, shown at 32 px, `max-age=0`. DejaVu TTF fonts 680 KB. Repo `logo.png` and `banner.png` raw reads 1.45 MB, `no-store`. lucide came from a third-party CDN, and `dashboard.js` waited for it (defer order). `tailwind.css` (render-blocking) and `analytics.js` were revalidated on every load. |
| 5 | **Worker-era routing.** Reads went to `nN.next.forkmesh.com` cross-origin (another TLS handshake); `raw` went through the Worker's router. `about` costs a dozen rqlite reads. | raw `logo.png` TTFB 300–1050 ms. `about` 208 ms. |

## What changed

| Commit | Change |
|---|---|
| c2d47d4 | Host fast path (`app/host/forkmesh_host/repo_fastpath.py`). Public reads (`tree`, `blob`, `blobs`, `raw`, `history`, `commit`, `compare`, `branches`, `search`, `stats`, `sizes`) are served from the node's gateway (`127.0.0.1:8790`), with the router's exact query and a verbatim body. Anything other than 200 falls through to the Worker; private-view requests never take this path. Commit-pinned reads are `immutable`; others get `no-cache` plus an ETag, and unchanged ones answer 304. A ref-less tree names its branch in `X-ForkMesh-Ref`. |
| 42303c8 | Gateway: commit-keyed response cache, single-flight on concurrent misses, and a split commit-graph with changed-path Bloom filters written at warmup (`FORKMESH_GATEWAY_COMMIT_GRAPH=0` turns it off). Directories with 65+ entries get their last-commit column from one `git log --name-only` walk. |
| b24113a | Early reads (`partials/repo-early-reads.html`) start the catalog, tree, README, file and history reads right after `api-client.js`, and the dashboard takes them over on an exact match. Reads stay same-origin when the origin reads locally, and revalidate instead of downloading again. WOFF2 fonts; srcset logo and SOL variants; one `Intl.NumberFormat`. |
| 74cc366 | Server-rendered first view (`repo_ssr.py`): tree, latest commit and README go into the HTML, plus their JSON, which the dashboard reuses. Gateway sets `TCP_NODELAY`. lucide is self-hosted (`/assets/vendor`, immutable). `tailwind.css`, `analytics.js` and the small images use `stale-while-revalidate`. |
| 8b639c0 | Anonymous `about` reads come from an in-process cache: 15 s fresh, then served stale while one background refresh runs, up to 5 min. |

## Numbers

Tree + README visible, in ms. The live "after" numbers will be re-measured after deploy.

| Page | Live before | Local before | Local after b24113a | Local after 74cc366 |
|---|---|---|---|---|
| repo, cold | tree 716–908, README 856–1029 (6103 after a push) | 566–628 / 651–795 | 486–623 (tree = README) | **220–244** |
| repo, warm | tree 327–415, README 377–489 | 438–502 / 539–583 | 435–564 | **168–220** |
| repo, hot (3rd load) | — | 397–441 / 494–504 | 405–425 | **140–164** |
| file view, cold / warm | 729–893 / 369–523 | 633–643 / 431–515 | 508–524 / 389–435 | (no SSR yet) |
| history, cold / warm | 730–874 / 318–404 | 596–642 / 396–419 | 455–517 / 375–400 | (no SSR yet) |

Other results:
- **Cold bytes:** 4.48 MB live, and 2.16 MB on the local "before" stack. After, 1.49 MB; most of that is `dashboard.js` plus the repo's own raw logo and banner, which are now 304 on revisits.
- **Gateway, locally:**
  - first `tree?path=&ref=main` after a restart: 1.5 s → 7 ms, because the warmed key now matches;
  - `desktop/src`: 64 s → 0.3 s;
  - `app/src`: 8.5 s → 0.1 s;
  - a cached read over keep-alive: 43 ms → 1.4 ms;
  - HTML with the server-rendered view: +3–5 ms at the node.
- **Live after b24113a** (operator box): HTML 92 ms, tree 79 ms, README 150 ms. The last two are dominated by the Nagle stall that 74cc366 removes.

## Deploy notes

- The fast path is on wherever `FORKMESH_NODE_ID` is set (`app.env`). Point it elsewhere with `FORKMESH_HOST_REPO_GATEWAY=http://…`, or turn it off with `off`.
- Restart the gateway on the new `tools/mirror_gateway.py`. Its first warmup writes the commit-graph once (a few seconds on a 1 vCPU node); later ones add a small layer per push.
- Install `brotli` into the deploy venv. `next_deploy.precompress` then also writes `.br` files, which Caddy's `precompressed br gzip` already prefers: about 20% less `dashboard.js` than gzip. Live currently serves gzip only because the module is missing.

## Still open

- **`dashboard.js` is 1.9 MB.** Compiling, booting and rendering it is ~150 ms of main-thread work before the full interface (not the server-rendered view) is interactive, more on phones. Splitting it per route is the next big lever; `06-repo-content.js` alone is 620 KB.
- **Other pages** (file view, history, issues, pulls, dashboard home) still render client-side only. They benefit from the fast path and the early reads, not from server rendering yet.
- **Anonymous first visits** await `/api/accounts/sessions` (`restoreSession`, one round trip) before the dashboard renders.
- **`sizes?ref=` is 380 KB of JSON** (57 KB zstd) for the About rail's size map, loaded on every repo view.
- **`tailwind.css` is not content-hashed.** `stale-while-revalidate` covers it, but hashing it needs the asset stamper and `test_asset_cache_busting` to accept sub-paths.


## Pass 2: pull requests, issues and discussions

Target: the pulls tab, a pull request (conversation and diff), the issues tab, an issue and the discussions tab usable in under 200 ms warm, measured like pass 1 (local stack, Chrome network emulation adding 30 ms per request; "usable" = the list rows or the record body visible).

### Before: the waterfall

The dashboard read these records from `.forkmesh` files one request after another, all after `dashboard.js` had booted (~400 ms):

- **Pulls tab** (live: 1.4–4.2 s):
  1. `tree .forkmesh/pulls @forkmesh/pulls` (the ledger commit);
  2. the same tree again, pinned to that commit;
  3. one batched `blobs` read of the 33 `pull.md` files in the ledger;
  4. one `blob` per pull whose own `forkmesh/pull/<n>` branch is ahead of the ledger, 4 at a time: **118 requests** for forkmesh/forkmesh (~30 sequential round trips).

  On live they went to a mirror origin (`n3.next…`, another TLS connection), because the page had no way to know that its own origin reads locally.
- **A pull request:** ledger tree → `pull.md` → `changes.patch` → pull folder tree + `commits.mbox` → comment blobs → `branches`: 6 sequential reads after boot (live 0.58–1.08 s).
- **Issues tab:** `.forkmesh/issues` tree → `closed/` tree → `open/` tree → open issue blobs (live 0.56–1.04 s). An issue: 1–3 blob probes (`open/`, `closed/`, legacy) plus the open-issue index (0.47–0.9 s). Discussions: tree → blobs (0.42–0.87 s).
- `GET /api/repo/<o>/<r>/pulls` and `/issues` answering 401 is not part of any of this: the pages never call them. `pulls` and `issues` under `/api/repo/` are the signed-event write routes (POST), and the Worker answers any other method on them with its auth check first. So it is not a fallback and costs nothing.
- **Session check:** `bootDashboard` awaited `/api/accounts/sessions` (a 401 for an anonymous visitor) before it rendered anything: one more round trip after `dashboard.js`.

### What changed

| Change | Where |
|---|---|
| **View bundles.** `GET /api/repo/<o>/<r>/page-reads?view=pulls\|pull\|issues\|issue\|discussions\|discussion[&number=N]` makes a view's reads against the node's gateway, in parallel, and returns them in one answer keyed exactly like the dashboard's own requests. Reads pinned to a commit come from a host-memory LRU (48 MB, immutable), so only the named-ref reads (`main`, `forkmesh/pulls`) reach the gateway on a rebuild: 7–17 ms warm, ~300 ms for the first pulls bundle after a restart. Concurrent builds are single-flight, a build is reused for 2 s, and a `?v=<content hash>` answer is immutable. Private and unknown repositories get 404 and read as before. | `app/host/forkmesh_host/repo_views.py`, `repo_fastpath.py` |
| **Server-rendered first view** of `/<o>/<r>/pulls`, `/pulls/<n>[/files\|/commits]`, `/issues`, `/issues/<n>`, `/discussions`, `/discussions/<n>`: the open list (first page, same order as the dashboard), or the record, its conversation and (on `/files`) the diff, capped at 60 files / 1,500 lines. A head script starts fetching the view's bundle at once, in parallel with `dashboard.js`. Budget 250 ms as for the repository page; otherwise the page is served as before. | `repo_ssr.py`, `repo_views.render` |
| **The dashboard takes its reads from the bundle** (`seededRepoRead` in `fetchRepoJson`): exact key match only. Commit-pinned entries stay valid; named-ref entries are used once, within a minute, so Refresh still reaches the node. In-app navigation (Code → Pull requests, a list row → a pull) primes the view's bundle in one request when the origin reads locally. A view page also marks the origin as local, so nothing goes to a mirror origin. | `06-repo-content.js`, `partials/repo-early-reads.html` |
| **Diffs:** a `compare` between two commit ids is immutable (browser and host memory), so a pull's diff survives the gateway's per-push cache generation. The first view renders the patch server-side; the dashboard's full diff view still parses the whole patch (bodies up to the gateway's compare cap are buffered, not streamed). | `repo_fastpath.cache_control` |
| **Session check before first render:** the page head starts `/api/accounts/sessions` (or `restoreSession` on the App hosts) while `dashboard.js` downloads; boot takes the answer over instead of asking after it. | `dashboard/shell.html`, `02-helpers.js` |

### Numbers (pass 2)

"Usable" is the first view (server-rendered, or the dashboard's own when there is none); "full" is the dashboard's interface with the data in it. Local medians of 3 runs, ms. Live "before" is two runs from the operator box (warm = HTML from the browser cache, `max-age=300`).

| Page | Live before (cold / warm) | Local before (cold / warm) | Local after: usable (cold / warm) | Local after: full (cold / warm) |
|---|---|---|---|---|
| pulls | 1741–4221 / 1436–1508 | 1995 / 1953 | 240 / 188 | 514 / 459 |
| pull #293 | 927–1075 / 582–659 | 774 / 780 | 252 / 200 | 546 / 500 |
| pull #293 files | 822–1011 / 676–698 | 766 / 784 | 268 / 212 | 590 / 518 |
| issues | 756–1041 / 558–601 | 610 / 600 | 236 / 188 | 462 / 453 |
| issue #702 | 787–901 / 467–532 | 599 / 586 | 228 / 188 | 535 / 425 |
| discussions | 609–873 / 415–442 | 532 / 526 | 212 / 176 | 466 / 414 |

In-app navigation (local, 30 ms RTT): Code → Pull requests 1571 → 162–302 ms; pulls → #293 335 → 173–187 ms; Code → Issues 224 → 99–107 ms. Requests per pulls view: 142 → 21 (one of them the bundle).

What still limits "usable" locally is first paint, not data: ~60–80 ms of style and layout of the dashboard shell after the HTML arrives (the same floor as the repository page). "Full" waits for `dashboard.js` to download, compile and boot (~100 ms of main thread warm).

### Deploy notes (pass 2)

- Host only (`app/host/forkmesh_host`) plus the rebuilt dashboard assets; no gateway change and no migration. Needs the fast path (`FORKMESH_NODE_ID` set, gateway on 127.0.0.1:8790), like pass 1.
- The first pulls bundle after an app restart takes ~300 ms locally (longer on a 1 vCPU node), so that one page is served without the rendered view; it is ready for the next visitor.
- Pages carrying a rendered view are `Cache-Control: no-cache` (they were `public, max-age=300` live), so a warm visit costs one round trip for the HTML and gets current data.

### After the first deploy of pass 2

Live right after the restart the pull and issue pages took 0.38–0.67 s for their HTML: each page waited (up to 250 ms) for a bundle build, a build ran on every request older than 2 s, the pull bundle included the gateway's `branches` listing (ahead/behind for ~150 branches), and the ~350 KB bundle was re-serialized on the event loop each time. Now:

- A view's bundle and the repository root's first view are remembered: served as is for 5 s, then served while one background build refreshes them (up to 5 min); a build that produces the same bytes keeps its rendered HTML.
- The page waits at most **40 ms** for a first view (was 250 ms); if nothing is remembered yet (after a restart) it is served as before, the dashboard reads the data itself, and the build finishes in the background for the next visitor.
- Bundles are the gateway's own JSON bytes joined, not re-serialized; `branches` is no longer in the pull bundle; the public check is kept 5 s.
- Local, app and gateway pinned to one contended core, 10 concurrent requests over the five pages: first round after restart median 105 ms (mostly unrendered, cold imports), then median 22–30 ms, max ≤ 54 ms, every page rendered; the stack before pass 2 measured median 15–17 ms, max 60–67 ms on the same test (repository root rendered only). The repository page path itself is unchanged by 3be0bd7/1d260a9; its 0.17–0.6 s right after the deploy matches the cold-start pattern (first request imports the Worker modules; the gateway rebuilds its caches and commit-graph after a restart).

### dashboard.js per page, and the catalog with the HTML

- **Per-page chunks.** The page modules only their own page runs (calendar, hackathon, notes, referrals, tasks) are served as `/dashboard-page-<name>.js` (immutable, content-hashed) and loaded when that page's init runs; their pages preload them with the HTML. `dashboard.js`: 1,978 KB → 1,753 KB (br 365 KB → 326 KB) on every other page. `app/src/dashboard_bundle.py` (`LAZY_CHUNKS`, `compose_split_from_reader`) computes each chunk's imports from the core and refuses a split the core reaches other than through the stubbed init (or that assigns a core `let`).
  A chunk script that fails to load (`script.onerror`: a dropped connection or a 5xx while a core restarts) is fetched once more after 1.5 s with `&retry=<time>`; if that fails too, the page shows a "Reload the page" notice and the promise still rejects, so a chunk that stays unreachable is still reported as `dashboard_chunk_failed`. Before 2026-09-28 one failed load rejected at once and the page stayed half-built with no notice (JS error group 520, `unhandledrejection [dashboard] dashboard_chunk_failed`, 2026-09-27 23:30). Tests: `app/tests/test_dashboard_bundle.py` runs the loader in node.
- **Why not smaller.** Everything else is one closure wired together by shared delegated handlers: a reachability pass from any single page's init reaches 98% of the code, and the repository fragments (06/07, ~850 KB) are called synchronously from the shared chrome (e.g. `if (handleRepoActionsClick(event)) return;`). A real "small core" needs those handlers moved behind per-page registration first; not done in this pass.
- **Catalog with the HTML.** Every dashboard page starts `/api/repositories` from its head (the repository page already did), and the dashboard takes it over (`takeEarlyPublicRead`).

Local, 30 ms RTT, medians of 3, "usable" = the page's data visible:

| Page | Live before (cold / warm) | Local before (cold / warm) | Local after (cold / warm) |
|---|---|---|---|
| home (`/dashboard`, feed) | 492–659 / 326–399 | 467 / 365 | 355 / 310 |
| repos (`/dashboard/repos`) | 517–607 / 274–344 | 449 / 355 | 355 / 310 |
| network (node list) | 453–498 / 346–378 | 224 / 168 (local has no nodes; first paint) | 220 / 204 |
| status (`/status`, www page) | 623–752 / 465–705 | — | not changed |
| login | — | 216 / 300 | 216 / 292 |

Not under 200 ms warm: home, repos, network and status still render their data client-side after `dashboard.js` (~100 ms of main thread) and, for network and status, their own API reads. Getting them there needs a server-rendered first view like the repository pages (the catalog is already in the node's memory), which this pass did not add. `/dashboard/settings` redirects anonymous visitors to `/login`; the profile lives on www (`next.forkmesh.com/@<name>`), outside this app.

### Compression

The stock Caddy 2.6 on the nodes has no dynamic Brotli encoder (only `gzip` and `zstd`; `br` exists only for precompressed files), so `encode zstd gzip` stays. Browsers already get zstd for HTML and JSON: Chrome and Firefox send `zstd`, and a `curl` that asks for gzip sees gzip. Static files are served precompressed as `.br`.

## Pass 3: the cores' "slow request" lines (2026-09-25)

The App host prints `slow request <ms> <method> <path>` for any request over
250 ms (`app/host/forkmesh_host/asgi.py`). The exception is `/api/runners/claim`: the app holds it open for up to 20 s on purpose (docs/RUNNERS.md). On 2026-09-24 the three cores
printed about 3,100 of them in 8 hours. The App itself was not the cost: its
unit used 2–4 % of a core's CPU on average, and a single request measured on
a core (`curl` to `127.0.0.1:8080`) answered in 5–30 ms. Slow lines come from
the single vCPU being shared with Git work, and from clients that send one
request per item in large bursts.

What the measurements showed:

- **CPU contention.** Each core has one vCPU. While a burst ran, `vmstat`
  showed 0 % idle (60–70 % user, the rest system), 50 MB/s of disk reads and
  swap traffic. `/proc/pressure/cpu` "some" averaged 33–47 % on n2 and n3.
  Over the uptime, `forkmesh-mirror-node.service` (the daemon, the Git gateway
  and their `git` children) used 25 % of n2's and n3's CPU on average, the
  largest share.
- **Catalog publishes.** Every catalog publish (`server/catalog.go`) ran two
  `git log --since=52 weeks ago --format=%ct` walks over forkmesh/forkmesh:
  3.3 s and 1.8 s of CPU on every core. These now run as one
  `git rev-list --timestamp --parents` pass that reads the commit-graph
  (1.0 s, same counts; see `docs/server/README.md`, Traps). Still open: the
  per-pull-branch `ls-tree` loop (152 refs, 1.5 s per publish), and
  `directorySize` walking the 3 GB `forkmesh-releases` store.
- **Account-profile bursts.** At 16:02 n2 logged 219 slow
  `GET /api/accounts/<name>` in one minute, each taking 23–31 s. One page load
  had asked for every contributor's profile at once. The World dressed each
  persisted contributor figure without a directory avatar through its own
  profile request, and the ledger made one profile request per person link.
  Both now take the avatar from the public directory (`/api/accounts/users`,
  one edge-cached read). A per-account lookup is left only for names the
  directory lacks, and the World waits for the directory before making any.
- **Pull-list fan-out (not fixed here).** The iOS and Android
  pull-request lists read `.forkmesh/pulls/<n>/pull.md`, plus a `tree` of each
  pull's directory, one request per pull. That is about 75 requests for 37
  pulls. They queue behind each other in the gateway: 1–3.5 s each, against
  100–400 ms alone. The fix belongs in the clients (use `blobs`, which takes
  up to 60 paths per request, or the dashboard's ledger reader). Native lives
  in the Next repository.
- **Catalog POSTs.** `POST /api/repositories` (398 slow, average 629 ms, max
  27 s) is the catalog record from the three cores and from desktops. The
  maximums line up with the bursts above and with rqlite elections during
  rolls.
- **rqlite `/status` every second.** The cluster view polled rqlite's full
  `/status` once a second on each core, and each call also logged the
  netlink error. That is fixed separately (the view reads `/leader`).

Rolls: at 17:51 an rqlite election during a rolling reboot (n1 restarted at
17:47, n2 rebooted at 17:51) made four requests time out after 15 s. They
became `503 d1 unavailable ... ReadTimeout` rows. These stay in `error_log`
on purpose (`log_d1_unavailable`): they are real unavailability, and /status
counts them separately from code faults. The fix is operational: roll a core
only once rqlite reports a stable leader with all voters reachable.
