"""The app.forkmesh.com/api landing page, its endpoint catalog, and the one
request counter the /status page still needs.

Per-minute API traffic statistics are no longer stored: the desktop's
Network → Web Requests tab counts them itself from the Worker log tail it
already streams (``wrangler tail``), so they cost D1 nothing and exist only
while someone is watching. The one survivor is FLAGSHIP_ROUTE_GROUP: the
/status sampler reads it every minute as visitor-side evidence that
forkmesh/forkmesh is not loading, so a failing flagship content read (HTTP
5xx) is counted in flagship_read_failure_minute — at most
FLAGSHIP_FAILURE_WRITES per isolate-minute, and nothing at all while the
repository serves.

The module also serves the developer surface of the landing page: an
endpoint catalog reflected from urls.py's route regexes (plus a curated list
for the literal routes entry.py matches inline) and a generated OpenAPI 3
document, so the page can offer a try-it request tester without a separately
maintained API description.

entry.py loads this module lazily (see the 10021 startup-memory budget notes
there); it must stay small and import nothing heavy at module scope.
"""

import re


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the metrics implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


MINUTE_MS = 60_000
# Failure rows exist only while the flagship repository is failing; a day is
# ample for the sampler, which reads the previous minute alone.
FLAGSHIP_FAILURE_RETENTION_MS = 24 * 3_600_000
_API_NAMESPACES = frozenset({
    "accounts", "admin", "ai", "attribution", "badges", "blog", "bot",
    "calendar", "chat", "client-errors",
    "bootstrap", "desktop-errors", "federation", "feedback", "forkbot", "genie",
    "install-diag", "install-source", "integrations", "ledger", "leaderboards",
    "mainnode", "metrics", "mirror-requests", "mirrors", "network",
    "nodes", "notes", "notifications", "openapi.json", "orgs",
    "poll", "private-replicas", "referrals", "relay-mesh", "repo",
    "repositories", "repository-imports", "rewards", "room", "security",
    "security-scans", "ssh", "status", "sync", "tasks", "version", "world",
})

_STATIC_GROUPS = frozenset({
    "accounts/availability", "accounts/change-password",
    "accounts/forgot-password", "accounts/login",
    "accounts/logout", "accounts/profile", "accounts/reset-password",
    "accounts/sessions", "accounts/signup", "accounts/users",
    "attribution/capture", "attribution/events", "attribution/session",
    "bootstrap/readiness", "metrics/endpoints", "mirrors/https",
    "network/overview", "openapi.json", "repositories", "status", "sync",
    "version", "world/client", "world/context", "world/inactive",
    "world/online", "world/qa", "world/ticket", "world/visitors",
    "world/wallet", "world/ws",
})

# The flagship repository's mirror-served content reads (the data behind
# https://forkmesh.com/forkmesh/forkmesh: tree/blob/raw/history/... and release
# blobs, i.e. the REPO_HOST_RE and RELEASE_BLOB_RE routes the page renders from)
# keep their own label instead of folding into the identifier-free "repo/*"
# group. forkmesh/forkmesh is the one repository the /status page monitors by
# name, so naming it here publishes no private identifier. A 5xx on one of
# these reads means a person could not load the repository page, whether or
# not the ten-minute deep probe has run yet (see
# _flagship_public_read_failures in status_monitoring.py, which keys on this
# exact label). Other flagship sub-routes (rooms, agents, mirrors, ...) are
# not repository content and stay in "repo/*".
FLAGSHIP_ROUTE_GROUP = "repo/forkmesh/forkmesh/content"
_FLAGSHIP_PATH_SEGMENTS = ("repo", "forkmesh", "forkmesh")
_FLAGSHIP_CONTENT_ACTIONS = frozenset({
    "tree", "blobs", "blob", "raw", "history", "commit", "compare",
    "branches", "search", "stats", "sizes",
})

# How many flagship 5xx responses per isolate-minute reach D1. The /status
# sampler calls the repository down at two or more failed reads in a minute
# (one stray timeout is not "the repository is not accessible"; a page load
# during an outage fails several reads at once) and reads the count at the
# next minute boundary, so two writes per isolate-minute are all the evidence
# it can use. Bounded: a failing repository costs at most this many writes per
# isolate-minute, never one per failed request.
FLAGSHIP_FAILURE_WRITES = 2

# (minute, failure writes already spent in it) for this isolate.
_flagship_failure_writes = (0, 0)


def _masked_group(path):
    """The identifier-free label for an /api path."""
    segments = [s for s in str(path or "").split("/") if s]
    if segments[:1] == ["api"]:
        segments = segments[1:]
    if not segments:
        return "(root)"
    lowered = [segment.lower() for segment in segments[:4]]
    namespace = lowered[0]
    if namespace not in _API_NAMESPACES:
        return "other"
    if tuple(lowered[:3]) == _FLAGSHIP_PATH_SEGMENTS and (
        (len(segments) == 4 and lowered[3] in _FLAGSHIP_CONTENT_ACTIONS)
        or [s.lower() for s in segments[3:5]] == ["releases", "blob"]
    ):
        return FLAGSHIP_ROUTE_GROUP
    candidate = "/".join(lowered)
    if candidate in _STATIC_GROUPS:
        return candidate
    return namespace if len(lowered) == 1 else namespace + "/*"


async def record_api_request(env, path, status, duration_ms=0):
    """Count a failed flagship content read for /status (never raises).

    Every other response is ignored: per-route traffic statistics live in the
    desktop, built from the Worker log tail, not in D1.
    """
    global _flagship_failure_writes
    try:
        if int(status or 0) < 500:
            return
        if _masked_group(path) != FLAGSHIP_ROUTE_GROUP:
            return
        now = int(Date.now())
        minute = (now // MINUTE_MS) * MINUTE_MS
        written_minute, spent = _flagship_failure_writes
        if written_minute != minute:
            spent = 0
        if spent >= FLAGSHIP_FAILURE_WRITES:
            return
        _flagship_failure_writes = (minute, spent + 1)
        await ensure_schema(env)
        await d1_run(
            env,
            "INSERT INTO flagship_read_failure_minute (minute_ts, failures) "
            "VALUES (?, 1) ON CONFLICT(minute_ts) DO UPDATE SET "
            "failures=failures+1",
            minute,
        )
        # Retention rides the first failure write of each isolate-minute; a
        # healthy repository writes nothing, so there is nothing to prune.
        if spent == 0:
            await d1_run(
                env,
                "DELETE FROM flagship_read_failure_minute WHERE minute_ts < ?",
                now - FLAGSHIP_FAILURE_RETENTION_MS,
            )
    except BaseException:
        # Status evidence must never break (or slow) serving; a dropped count
        # is bounded loss.
        pass


# Literal routes entry.py matches inline (they have no urls.py regex to
# reflect). Kept deliberately short: the reflected table below carries the
# bulk.
_CURATED_ENDPOINTS = [
    ("GET", "/api/version", "Build stamp + which Worker answered"),
    ("GET", "/api/status", "Public systems status and minute history"),
    ("GET", "/api/metrics/endpoints", "This endpoint catalog"),
    ("GET", "/api/openapi.json", "Generated OpenAPI 3 description"),
    ("GET", "/api/turnstile", "Public Turnstile site key for account forms"),
    ("GET", "/api/repositories", "Public repository catalog"),
    ("GET", "/api/repositories/search", "Ranked, paginated public index"),
    ("GET", "/api/ledger", "Public financial system of record"),
    ("GET", "/api/mirrors/https", "Mirror gateway protocol identity"),
    ("GET", "/api/preservation/crisis-plan", "Crisis preservation plan or scenario catalog"),
    ("GET", "/api/world/online", "World presence count"),
    ("GET", "/api/world/deploy-status", "World deploy semaphore"),
    ("GET", "/api/accounts/users", "Public account directory"),
    ("GET", "/api/leaderboards", "Node leaderboards"),
    ("GET", "/api/sync", "Signed owner-node drain (owner+ts+sig auth)"),
]


def _template_from_pattern(pattern):
    """A human-usable path template for one urls.py route regex."""
    text = str(pattern or "")
    text = text[1:] if text.startswith("^") else text
    text = text[:-1] if text.endswith("$") else text
    text = text.replace("/?", "")
    # Keep the first alternative of non-capturing (?:a|b) groups.
    text = re.sub(r"\(\?:([^)|]+)(?:\|[^()]*)?\)", r"\1", text)
    counter = {"n": 0}

    def _placeholder(_match):
        counter["n"] += 1
        return "{p%d}" % counter["n"]

    text = re.sub(r"\((?:[^()]|\([^()]*\))*\)", _placeholder, text)
    text = text.replace("\\.", ".").replace("\\-", "-")
    if text.startswith("/api/repo/{p1}/{p2}"):
        text = text.replace("{p1}", "{owner}", 1).replace("{p2}", "{repo}", 1)
    elif text.startswith("/api/orgs/{p1}"):
        text = text.replace("{p1}", "{org}", 1)
    return text


def endpoint_catalog():
    """Every route template: reflected from urls.py plus the curated list."""
    import urls
    items = {}
    for method, template, summary in _CURATED_ENDPOINTS:
        items[template] = {
            "method": method, "template": template, "summary": summary,
            "source": "curated",
        }
    for name in sorted(dir(urls)):
        if not name.endswith("_RE"):
            continue
        pattern = str(getattr(getattr(urls, name), "pattern", "") or "")
        if not pattern.startswith("^/"):
            continue
        template = _template_from_pattern(pattern)
        if template in items:
            continue
        method = "POST" if (
            "upload-pack" in template or "receive-pack" in template
        ) else "GET"
        items[template] = {
            "method": method,
            "template": template,
            "summary": name[:-3].replace("_", " ").lower(),
            "source": "urls.py",
        }
    catalog = []
    for item in items.values():
        path_only = item["template"].split("?")[0]
        item["group"] = (
            _masked_group(path_only)
            if path_only.startswith("/api/") else None)
        item["params"] = re.findall(r"\{([a-z0-9]+)\}", item["template"])
        catalog.append(item)
    catalog.sort(key=lambda i: i["template"])
    return catalog


async def endpoint_catalog_handler(env, request):
    """GET /api/metrics/endpoints — the catalog the landing page renders."""
    return json_response({
        "ok": True,
        "endpoints": endpoint_catalog(),
    }, cache_control="public, max-age=300")


async def openapi_handler(env, request):
    """GET /api/openapi.json — a generated, best-effort OpenAPI 3 document.

    Paths and parameters are exact (reflected from the route table); methods
    beyond the known POST routes default to GET, and auth/response schemas
    are deliberately not asserted — this exists so standard tooling can
    import the surface, not as a hand-maintained contract.
    """
    paths = {}
    for item in endpoint_catalog():
        template = item["template"].split("?")[0]
        operation = {
            "summary": item["summary"],
            "responses": {"200": {"description": "OK"}},
        }
        if item["params"]:
            operation["parameters"] = [
                {
                    "name": param,
                    "in": "path",
                    "required": True,
                    "schema": {"type": "string"},
                }
                for param in item["params"]
            ]
        paths.setdefault(template, {})[item["method"].lower()] = operation
    return json_response({
        "openapi": "3.0.3",
        "info": {
            "title": "ForkMesh API",
            "version": str(getattr(env, "APP_VERSION", "") or "0"),
            "description": (
                "Generated from the Worker's route table. Methods beyond "
                "the known POST routes default to GET; many routes require "
                "signed or session auth that this document does not model."),
        },
        "servers": [{
            "url": str(
                getattr(env, "API_ORIGIN", "")
                or "https://app.forkmesh.com"
            ).rstrip("/")
        }],
        "paths": paths,
    }, cache_control="public, max-age=300")


def landing_page_response(env):
    """The app /api landing page: endpoint catalog and request tester.

    Self-contained (inline CSS/JS, no CDNs — the page sets its own CSP) and
    dark-committed: it is an operations surface, not a marketing page. Live
    traffic charts are the desktop's Network → Web Requests tab, built from
    the Worker log tail; nothing here polls.
    """
    rev = str(getattr(env, "BUILD_REV", "") or "")[:12]
    worker = str(getattr(env, "WORKER_ROLE", "") or "relay")
    html = _LANDING_PAGE_HTML.replace("__REV__", rev).replace(
        "__WORKER__", worker)
    return Response(html, status=200, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store, max-age=0, must-revalidate",
        "content-security-policy": (
            "default-src 'none'; script-src 'unsafe-inline'; "
            "style-src 'unsafe-inline'; connect-src 'self'; "
            "img-src 'self' data:; base-uri 'none'; form-action 'none'"),
        "x-frame-options": "DENY",
        "x-content-type-options": "nosniff",
        "referrer-policy": "strict-origin-when-cross-origin",
    })


_LANDING_PAGE_HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex">
<title>ForkMesh API</title>
<style>
  :root {
    color-scheme: dark;
    --surface: #1a1a19; --panel: #232322; --line: #ffffff14;
    --text: #ffffff; --text-2: #c3c2b7; --text-3: #8b8a80;
    --series-req: #3987e5;
  }
  * { box-sizing: border-box; margin: 0; }
  body { background: var(--surface); color: var(--text);
    font: 14px/1.5 ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    padding: 24px; max-width: 1080px; margin: 0 auto; }
  header { display: flex; flex-wrap: wrap; align-items: baseline;
    gap: 12px; margin-bottom: 20px; }
  h1 { font-size: 18px; font-weight: 700; }
  h2 { font-size: 13px; font-weight: 600; color: var(--text-2);
    margin: 26px 0 10px; text-transform: uppercase; letter-spacing: .06em; }
  .meta { color: var(--text-3); font-size: 12px; }
  button { background: var(--panel); color: var(--text-2); cursor: pointer;
    border: 1px solid var(--line); border-radius: 6px; padding: 4px 12px;
    font: inherit; font-size: 12px; }
  .panel { background: var(--panel); border-radius: 8px; padding: 14px;
    overflow-x: auto; }
  table { border-collapse: collapse; width: 100%; font-size: 12px; }
  th, td { text-align: right; padding: 5px 10px;
    border-bottom: 1px solid var(--line); }
  th:first-child, td:first-child { text-align: left; }
  th { color: var(--text-3); font-weight: 600; }
  a { color: var(--series-req); }
  .hidden { display: none; }
</style>
</head>
<body>
<header>
  <h1>ForkMesh API</h1>
  <span class="meta">served by __WORKER__ · build __REV__</span>
</header>

<h2 id="catalogHead">API endpoints
  <span class="meta" style="text-transform:none;letter-spacing:0">
    — reflected from the route table ·
    <a href="/api/openapi.json">openapi.json</a></span></h2>
<div class="panel">
  <input id="epFilter" placeholder="filter endpoints…" aria-label="filter
    endpoints" style="width:100%;margin-bottom:10px;background:var(--surface);
    border:1px solid var(--line);border-radius:6px;color:var(--text);
    font:inherit;padding:6px 10px">
  <div style="max-height:340px;overflow-y:auto">
    <table id="catalogTable"><thead><tr><th>method</th><th>endpoint</th>
      <th></th></tr></thead>
      <tbody></tbody></table>
  </div>
</div>

<h2 id="testerHead">Request tester</h2>
<div class="panel" id="tester">
  <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px">
    <select id="tMethod" aria-label="method" style="background:var(--surface);
      color:var(--text);border:1px solid var(--line);border-radius:6px;
      font:inherit;padding:6px">
      <option>GET</option><option>POST</option><option>PUT</option>
      <option>DELETE</option><option>HEAD</option>
    </select>
    <input id="tPath" value="/api/version" aria-label="request path"
      style="flex:1;min-width:280px;background:var(--surface);
      border:1px solid var(--line);border-radius:6px;color:var(--text);
      font:inherit;padding:6px 10px">
    <button id="tSend" style="border-color:var(--series-req);
      color:var(--text)">send</button>
  </div>
  <div style="display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px">
    <input id="tAuth" placeholder="bearer token (optional)"
      aria-label="bearer token" style="flex:1;min-width:280px;
      background:var(--surface);border:1px solid var(--line);
      border-radius:6px;color:var(--text);font:inherit;padding:6px 10px">
  </div>
  <textarea id="tBody" rows="3" placeholder="request body (non-GET, JSON)"
    class="hidden" style="width:100%;background:var(--surface);
    border:1px solid var(--line);border-radius:6px;color:var(--text);
    font:inherit;padding:6px 10px;margin-bottom:8px"></textarea>
  <div class="meta" id="tStatus">Pick an endpoint above or type a path, then
    send. Requests run same-origin against this Worker with your token only
    in this tab.</div>
  <pre id="tOut" class="hidden" style="background:var(--surface);
    border-radius:6px;padding:10px;max-height:360px;overflow:auto;
    white-space:pre-wrap;font-size:12px;margin-top:8px"></pre>
</div>

<p class="meta" style="margin-top:18px">Per-route traffic is not recorded
here; the ForkMesh desktop counts it live from the Worker log.</p>

<script>
"use strict";
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"']/g,
  (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));

// --- Endpoint catalog + request tester ---------------------------------
let catalog = [];
async function loadCatalog() {
  try {
    const r = await fetch("/api/metrics/endpoints");
    catalog = (await r.json()).endpoints || [];
    renderCatalog();
  } catch (e) { /* transient */ }
}
function renderCatalog() {
  const needle = $("epFilter").value.trim().toLowerCase();
  const rows = catalog
    .filter((e) => !needle || e.template.toLowerCase().includes(needle) ||
      (e.summary || "").toLowerCase().includes(needle))
    .map((e) =>
      `<tr><td style="text-align:left">${esc(e.method)}</td>` +
      `<td style="text-align:left" title="${esc(e.summary || "")}">` +
      `${esc(e.template)}</td>` +
      `<td><button data-try="${esc(e.template)}" ` +
      `data-method="${esc(e.method)}">try</button></td></tr>`).join("");
  $("catalogTable").querySelector("tbody").innerHTML =
    rows || `<tr><td colspan="3">no matches</td></tr>`;
  $("catalogTable").querySelectorAll("button[data-try]").forEach((b) => {
    b.onclick = () => {
      $("tMethod").value = b.dataset.method;
      $("tPath").value = b.dataset.try.split("?")[0];
      $("tBody").classList.toggle("hidden", b.dataset.method === "GET");
      $("tester").scrollIntoView({ behavior: "smooth", block: "center" });
      $("tPath").focus();
    };
  });
}
$("epFilter").oninput = renderCatalog;
$("tMethod").onchange = () =>
  $("tBody").classList.toggle("hidden", $("tMethod").value === "GET");
$("tSend").onclick = async () => {
  const method = $("tMethod").value;
  let path = $("tPath").value.trim();
  if (!path.startsWith("/")) path = "/" + path;
  const headers = { accept: "application/json" };
  if ($("tAuth").value.trim())
    headers.authorization = "Bearer " + $("tAuth").value.trim();
  const init = { method, headers, cache: "no-store" };
  if (method !== "GET" && method !== "HEAD" && $("tBody").value.trim()) {
    init.body = $("tBody").value;
    headers["content-type"] = "application/json";
  }
  $("tStatus").textContent = "…";
  const started = performance.now();
  try {
    const r = await fetch(path, init);
    const ms = Math.round(performance.now() - started);
    const text = await r.text();
    let body = text;
    try { body = JSON.stringify(JSON.parse(text), null, 2); } catch (e) {}
    const shown = body.length > 20000
      ? body.slice(0, 20000) + "\\n… (" + body.length + " chars)" : body;
    const hdrs = [...r.headers.entries()].map(([k, v]) => k + ": " + v)
      .join("\\n");
    $("tStatus").textContent =
      `HTTP ${r.status} · ${ms} ms · ${text.length} bytes — ` +
      `curl -X ${method} '${location.origin}${path}'`;
    $("tOut").classList.remove("hidden");
    $("tOut").textContent = hdrs + "\\n\\n" + shown;
  } catch (e) {
    $("tStatus").textContent = "request failed: " + e;
  }
};

loadCatalog();
</script>
</body>
</html>
"""
