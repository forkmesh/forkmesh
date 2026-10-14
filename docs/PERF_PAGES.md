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
| **Static weight.** Self-hosted icons, small logos, precompressed assets (`.br` when `brotli` is in the deploy venv, plus gzip), immutable caching for hashed files, `no-cache` for pages carrying a rendered view. | build, Caddy |

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
- The About rail's size map is a large JSON read on every repository view.
- `tailwind.css` is not content-hashed.
