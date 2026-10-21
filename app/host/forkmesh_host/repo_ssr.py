"""Server-rendered first view of a repository page (docs/PERF_REPO_PAGE.md).

The repository page is the dashboard shell plus 1.9 MB of dashboard.js,
which renders everything client-side, so nothing of the repository showed
until that script had downloaded, compiled and run. On a node that reads the
repository locally (repo_fastpath) the host now adds the first view to the
HTML the Worker returns for ``/<owner>/<repo>``:

* the root file list, the latest commit and the README, rendered into the
  ``[data-repo-detail]`` placeholder with a few lines of inline CSS, so the
  page is readable after one round trip. dashboard.js replaces the block
  with the full interface when it has booted;
* the same tree and README as JSON (``#forkmesh-repo-ssr``), which the
  page's early-read script (partials/repo-early-reads.html) hands to the
  dashboard, so it does not ask for them again.

An issue page (``/<owner>/<repo>/issues/<n>``) gets the header, the tab
strip and the issue itself in the dashboard's own markup (repo_ssr_issue,
from repo_views' issue bundle), which dashboard.js keeps in place until its
own render of the same markup replaces it.

The reads start when the request arrives, in parallel with the Worker, and
are skipped (the page is served as before) when they take longer than
``SSR_BUDGET_SECONDS`` or the gateway does not serve the repository publicly.
A page carrying rendered data is sent ``Cache-Control: no-cache``.
"""

from __future__ import annotations

import asyncio
import datetime
import html
import json
import re
import time
import urllib.parse

PAGE_RE = re.compile(r"^/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})/?$")
SHORT_PAGE_RE = re.compile(r"^/([A-Za-z0-9_-][A-Za-z0-9._-]{0,99})/?$")
SHORT_META_RE = re.compile(
    r'<meta name="forkmesh-repo-(short|owner|canonical)" content="([^"]*)">')
RESERVED_OWNERS = frozenset({
    "api", "assets", "dashboard", "favicon", "docs", "blog", "world", "login",
    "signup", "static", "ap", "nodeinfo", "admin", "hackathon", "notes",
    "referral", "referrals", "microjobs", "status", "start", "chat",
    "pitch-deck", "product-analysis", "network", "ledger", "leaderboards",
})
README_RE = re.compile(r"^readme(\.md|\.txt|\.rst)?$", re.I)
# The page waits at most this long for its first view; after that it is
# served as before and the dashboard reads the data itself. Views and the
# repository root are remembered (stale-while-revalidate), so the wait is
# only ever paid when nothing is remembered yet (after a restart).
SSR_BUDGET_SECONDS = 0.04
PRERENDER_FRESH_SECONDS = 5.0
PRERENDER_STALE_SECONDS = 300.0
MAX_TREE_ROWS = 400
MAX_README_CHARS = 200_000
PLACEHOLDER = ('<div data-repo-frame data-repo-layout="pending" class="min-w-0"><div '
               'data-repo-github-header class="h-12 border-b border-border"></div><p '
               'class="sr-only">Choose a repository to inspect it.</p></div>')
CHARSET = '<meta charset="UTF-8" />'

STYLE = """<style>
.fm-ssr{max-width:1012px;margin:0 auto;padding:24px 16px;font-size:14px;line-height:1.5;color:rgb(var(--dashboard-foreground-rgb))}
.fm-ssr a{color:var(--dashboard-link,#58a6ff);text-decoration:none}.fm-ssr a:hover{text-decoration:underline}
.fm-ssr-title{font-size:20px;font-weight:600;margin:0 0 16px}
.fm-ssr-box{border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;overflow:hidden;margin-bottom:16px;background:rgb(var(--dashboard-card-rgb))}
.fm-ssr-commit{display:flex;gap:8px;align-items:center;padding:10px 16px;background:rgb(var(--dashboard-subtle-rgb,22 27 34));border-bottom:1px solid rgb(var(--dashboard-border-rgb));color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-ssr-commit b{color:rgb(var(--dashboard-foreground-rgb));font-weight:600}.fm-ssr-commit span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0}
.fm-ssr-row{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,2fr) auto;gap:16px;padding:7px 16px;border-top:1px solid rgb(var(--dashboard-border-rgb))}
.fm-ssr-row:first-child{border-top:0}.fm-ssr-row>*{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fm-ssr-row .m,.fm-ssr-row time{color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-ssr-readme h2.fm-ssr-file{font-size:14px;margin:0;padding:10px 16px;border-bottom:1px solid rgb(var(--dashboard-border-rgb))}
.fm-ssr-md{padding:16px 24px;overflow-wrap:anywhere}.fm-ssr-md h1,.fm-ssr-md h2{border-bottom:1px solid rgb(var(--dashboard-border-rgb));padding-bottom:.3em}
.fm-ssr-md h1{font-size:2em;margin:.67em 0}.fm-ssr-md h2{font-size:1.5em;margin:1em 0 .5em}.fm-ssr-md h3{font-size:1.25em;margin:1em 0 .5em}
.fm-ssr-md pre{background:rgb(var(--dashboard-muted-rgb));padding:12px;border-radius:6px;overflow:auto;font-family:ui-monospace,monospace;font-size:12px}
.fm-ssr-md code{font-family:ui-monospace,monospace;font-size:85%}.fm-ssr-md ul,.fm-ssr-md ol{padding-left:2em}
.fm-ssr-md blockquote{margin:0;padding:0 1em;border-left:.25em solid rgb(var(--dashboard-border-rgb));color:rgb(var(--dashboard-muted-foreground-rgb))}
@media (max-width:640px){.fm-ssr-row{grid-template-columns:minmax(0,1fr) auto}.fm-ssr-row .m{display:none}}
[data-repo-ssr] [role=tab][aria-selected=true]{font-weight:600}[data-repo-ssr] [role=tab][aria-selected=true]::after{content:"";position:absolute;left:8px;right:8px;bottom:0;height:2px;border-radius:6px;background:var(--dashboard-tab-underline)}
.fm-ssr-readme{overflow-wrap:anywhere}.fm-ssr-readme>:first-child{margin-top:0}.fm-ssr-readme>:last-child{margin-bottom:0}
.fm-ssr-readme h1,.fm-ssr-readme h2,.fm-ssr-readme h3,.fm-ssr-readme h4,.fm-ssr-readme h5,.fm-ssr-readme h6{font-weight:600;margin:1.25rem 0 .5rem}
.fm-ssr-readme h1{font-size:1.5rem;line-height:2rem}.fm-ssr-readme h2{font-size:1.25rem;line-height:1.75rem}.fm-ssr-readme h3{font-size:1.125rem;line-height:1.75rem}.fm-ssr-readme h4{font-size:1rem}.fm-ssr-readme h6{font-size:.75rem}
.fm-ssr-readme p{margin:.5rem 0;line-height:1.5rem}.fm-ssr-readme ul,.fm-ssr-readme ol{margin:.5rem 0 .5rem 1.25rem}.fm-ssr-readme ul{list-style:disc}.fm-ssr-readme ol{list-style:decimal}.fm-ssr-readme li+li{margin-top:.25rem}
.fm-ssr-readme a{color:var(--dashboard-link,#58a6ff);text-decoration:underline;text-underline-offset:2px}.fm-ssr-readme hr{margin:1rem 0;border-color:rgb(var(--dashboard-border-rgb))}
.fm-ssr-readme code{border-radius:.25rem;background:rgb(var(--dashboard-secondary-rgb));padding:.125rem .375rem;font-family:ui-monospace,monospace;font-size:.75rem}
.fm-ssr-readme pre{margin:.75rem 0;overflow:auto;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:.375rem;padding:.75rem;font-size:.75rem}.fm-ssr-readme pre code{background:none;padding:0}
.fm-ssr-readme blockquote{margin:.75rem 0;border-left:2px solid rgb(var(--dashboard-border-rgb));padding-left:.75rem;color:rgb(var(--dashboard-muted-foreground-rgb))}
</style>"""


def page_target(scope):
    """(owner, repo) when this GET may be a repository page, else None."""
    if scope.get("method", "GET").upper() != "GET":
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    match = PAGE_RE.match(path)
    if not match or match.group(1).lower() in RESERVED_OWNERS:
        return None
    if (scope.get("query_string") or b""):
        return None  # ?viewer=/?sig= private views, ?ref= branch picks
    repo = match.group(2)
    if repo in (".", "..") or repo.endswith(".git"):
        return None
    return match.group(1).lower(), repo


def short_target(scope):
    if scope.get("method", "GET").upper() != "GET":
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    match = SHORT_PAGE_RE.match(path)
    if not match or (scope.get("query_string") or b""):
        return None
    name = match.group(1)
    if name.lower() in RESERVED_OWNERS or name.endswith(".git"):
        return None
    return name


def short_page(page):
    found = {}
    end = page.find("</head>")
    for key, value in SHORT_META_RE.findall(page if end < 0 else page[:end]):
        found.setdefault(key, html.unescape(value))
    owner = found.get("owner", "").lower()
    repo = found.get("short", "")
    if not PAGE_RE.match("/%s/%s" % (owner, repo)):
        return None
    return owner, repo, found.get("canonical", "").lower() == owner


async def _get_json(fastpath, url):
    response = await fastpath.client().get(url)
    if response.status_code != 200:
        return None
    return response.json()


async def prerender(fastpath, owner, repo):
    """Tree, default branch and README from the local gateway, or None."""
    base = "/v1/public/repositories/%s/%s/" % (
        urllib.parse.quote(owner, safe=""), urllib.parse.quote(repo, safe=""))
    try:
        tree = await _get_json(fastpath, base + "tree?path=")
        if not tree or not tree.get("commit"):
            return None
        commit = str(tree["commit"])
        ref = await fastpath.default_branch(owner, repo, commit)
        if not ref:
            return None
        readme = None
        for entry in tree.get("entries") or []:
            if entry.get("type") == "blob" and README_RE.match(
                    str(entry.get("name") or "")):
                blob = await _get_json(fastpath, base + "blob?" + urllib.parse.urlencode(
                    [("path", entry["name"]), ("ref", commit)]))
                if blob and blob.get("ok") is not False:
                    readme = {"name": entry["name"], "data": blob}
                break
        info = None
        if any(entry.get("type") == "tree" and entry.get("name") == ".forkmesh"
               for entry in tree.get("entries") or []):
            blob = await _get_json(fastpath, base + "blob?" + urllib.parse.urlencode(
                [("path", ".forkmesh/info.json"), ("ref", commit)]))
            info = repo_info(blob)
        node = await fastpath.node_name() or ""
        return {"owner": owner, "repo": repo, "ref": ref, "commit": commit,
                "servedBy": node, "tree": tree, "readme": readme, "info": info}
    except Exception:
        return None


def repo_info(blob):
    if not isinstance(blob, dict) or blob.get("ok") is False \
            or blob.get("encoding") == "base64":
        return None
    try:
        info = json.loads(str(blob.get("content") or ""))
    except ValueError:
        return None
    if not isinstance(info, dict):
        return None
    about = info.get("about")
    website = info.get("website")
    return {"about": about.strip()[:500] if isinstance(about, str) else "",
            "website": website.strip()[:500] if isinstance(website, str) else ""}


class ShortTicket:
    def __init__(self, fastpath, name):
        self.fastpath = fastpath
        self.name = name


class ViewTicket:
    """A pull request, issue or discussion page being rendered
    (repo_views): the bundle build and the tab it opens on."""

    def __init__(self, task, section, target=None, root=None):
        self.task = task
        self.section = section
        self.target = target
        self.root = root


ISSUE_BUDGET_SECONDS = 0.3


async def _within(task, seconds):
    try:
        return await asyncio.wait_for(asyncio.shield(task), max(0.01, seconds))
    except asyncio.TimeoutError:
        return None
    except Exception:
        return None


_prerendered = {}   # (owner, repo) -> (monotonic time, data)
_prerendering = {}  # (owner, repo) -> Future


def _prerender_task(fastpath, owner, repo):
    key = (owner, repo)
    pending = _prerendering.get(key)
    if pending is None:
        async def run():
            data = await prerender(fastpath, owner, repo)
            if data:
                if len(_prerendered) > 256:
                    _prerendered.clear()
                _prerendered[key] = (asyncio.get_running_loop().time(), data)
            else:
                _prerendered.pop(key, None)
            return data
        pending = asyncio.ensure_future(run())
        _prerendering[key] = pending
        pending.add_done_callback(lambda _f: _prerendering.pop(key, None))
    return pending


def remembered_prerender(fastpath, owner, repo):
    """A future for the repository root's first view: the remembered one at
    once when there is one (refreshed in the background once stale)."""
    entry = _prerendered.get((owner, repo))
    loop = asyncio.get_running_loop()
    if entry is not None and loop.time() - entry[0] < PRERENDER_STALE_SECONDS:
        if loop.time() - entry[0] >= PRERENDER_FRESH_SECONDS:
            _prerender_task(fastpath, owner, repo)
        done = loop.create_future()
        done.set_result(entry[1])
        return done
    return _prerender_task(fastpath, owner, repo)


def start(fastpath, scope):
    """Begin the reads for a possible repository page (or return None).

    For an anonymous read of a slow public Worker route (MICRO_CACHE_RE)
    the ticket is instead the micro-cache key, see fetch()."""
    if fastpath is None:
        return None
    key = micro_cache_key(scope)
    if key is not None:
        return key
    from . import repo_views
    view = repo_views.view_target(scope)
    if view is not None:
        owner, repo, name, number, section = view
        bundle = repo_views.cached(fastpath, owner, repo, name, number)
        if bundle is not None:
            task = asyncio.get_running_loop().create_future()
            task.set_result(bundle)
        else:
            task = asyncio.ensure_future(repo_views.build(
                fastpath, owner, repo, name, number))
        root = remembered_prerender(fastpath, owner, repo) \
            if name == "issue" and not section else None
        return ViewTicket(task, section, (owner, repo, name, number), root)
    target = page_target(scope)
    if target is None:
        name = short_target(scope)
        return ShortTicket(fastpath, name) if name else None
    return remembered_prerender(fastpath, *target)


# -- micro-cache for slow public Worker reads ------------------------------
# /api/repo/<o>/<r>/about is a dozen rqlite queries (catalog row, group
# resolution, media, logo, fediverse stats): ~200 ms on a 1 vCPU node, for
# an answer that is the same for every anonymous visitor. Anonymous GETs are
# answered from memory for MICRO_FRESH_SECONDS, then served stale while one
# background request refreshes them, for up to MICRO_STALE_SECONDS. A signed
# in visitor (cookie or Authorization) always reaches the Worker, and a
# write to the same path through this node drops the entry.
MICRO_CACHE_RE = re.compile(r"^/api/repo/[^/]+/[^/]+/about$")
MICRO_FRESH_SECONDS = 15.0
MICRO_STALE_SECONDS = 300.0
MICRO_MAX_ENTRIES = 512
MICRO_MAX_BODY = 256 * 1024
_micro = {}
_micro_refreshing = set()


class MicroKey(str):
    pass


def micro_cache_key(scope):
    path = scope.get("path") or ""
    if not MICRO_CACHE_RE.match(path):
        return None
    method = scope.get("method", "GET").upper()
    if method != "GET":
        _micro.pop(path, None)  # a write: the next read goes to the Worker
        for key in [k for k in _micro if k.startswith(path + "?")]:
            _micro.pop(key, None)
        return None
    for name, _value in scope.get("headers") or []:
        if name in (b"cookie", b"authorization"):
            return None
    query = (scope.get("query_string") or b"").decode("latin-1")
    return MicroKey(path + ("?" + query if query else ""))


def _micro_response(entry):
    from . import js as _js
    status, headers, body = entry[1], entry[2], entry[3]
    return _js.Response(body, status, "", _js.Headers(dict(headers)))


async def _micro_store(key, response):
    from . import js as _js
    body = await response._read()
    headers = [(k, v) for k, v in response.headers.raw()
               if k not in ("set-cookie", "content-length")]
    if int(response.status) == 200 and len(body) <= MICRO_MAX_BODY:
        if len(_micro) >= MICRO_MAX_ENTRIES:
            _micro.clear()
        _micro[key] = (asyncio.get_running_loop().time(), 200, headers, body)
    return _js.Response(body, response.status, "", _js.Headers(dict(headers)))


async def fetch(ticket, worker_fetch, request):
    """The Worker's answer, or the micro-cache's for an anonymous read."""
    if not isinstance(ticket, MicroKey):
        return await worker_fetch(request)
    entry = _micro.get(ticket)
    now = asyncio.get_running_loop().time()
    if entry is not None and now - entry[0] < MICRO_STALE_SECONDS:
        if now - entry[0] >= MICRO_FRESH_SECONDS \
                and ticket not in _micro_refreshing:
            _micro_refreshing.add(ticket)

            async def refresh():
                try:
                    await _micro_store(ticket, await worker_fetch(request))
                except Exception:
                    pass
                finally:
                    _micro_refreshing.discard(ticket)
            asyncio.ensure_future(refresh())
        return _micro_response(entry)
    return await _micro_store(ticket, await worker_fetch(request))


async def finish(task, response):
    """Add the rendered first view to the Worker's repository page."""
    if task is None or isinstance(task, MicroKey):
        return response
    view = task if isinstance(task, ViewTicket) else None
    if view is not None:
        task = view.task
    try:
        content_type = str(response.headers.get("content-type") or "")
        if int(response.status) != 200 or "text/html" not in content_type:
            return response
        from . import js as _js
        if isinstance(task, ShortTicket):
            return await _finish_short(task, response)
        started = asyncio.get_running_loop().time()
        try:
            data = await asyncio.wait_for(asyncio.shield(task), ISSUE_BUDGET_SECONDS
                                          if view is not None and view.root is not None
                                          else SSR_BUDGET_SECONDS)
        except asyncio.TimeoutError:
            if view is None or view.target is None:
                return response
            body = await response._read()
            rendered = inject_view_reads(body.decode("utf-8"), *view.target)
            if rendered is None:
                return _js.Response(body, response.status, "",
                                    response.headers)
            headers = response.headers
            headers.set("cache-control", "no-cache")
            return _js.Response(rendered.encode("utf-8"), response.status, "",
                                headers)
        if not data:
            return response
        body = await response._read()
        page = body.decode("utf-8")
        rendered = None
        if view is not None and view.root is not None:
            root = await _within(view.root, ISSUE_BUDGET_SECONDS - (
                asyncio.get_running_loop().time() - started))
            if root:
                rendered = inject_issue(page, root, data)
        if rendered is None:
            rendered = inject_view(page, data, view.section) if view \
                else inject(page, data)
        if rendered is None:
            return _js.Response(body, response.status, "", response.headers)
        headers = response.headers
        headers.set("cache-control", "no-cache")
        return _js.Response(rendered.encode("utf-8"), response.status, "",
                            headers)
    except Exception as error:  # the plain page is always a valid answer
        print("repo ssr skipped: %r" % (error,))
        return response


async def _finish_short(ticket, response):
    from . import js as _js
    body = await response._read()
    page = body.decode("utf-8")
    named = short_page(page)
    rendered = None
    if named is not None and named[1].lower() == ticket.name.lower():
        owner, repo, canonical = named
        try:
            data = await asyncio.wait_for(asyncio.shield(remembered_prerender(
                ticket.fastpath, owner, repo)), SSR_BUDGET_SECONDS)
        except asyncio.TimeoutError:
            data = None
        if data:
            rendered = inject(page, data, short=canonical)
    if rendered is None:
        return _js.Response(body, response.status, "", response.headers)
    headers = response.headers
    headers.set("cache-control", "no-cache")
    return _js.Response(rendered.encode("utf-8"), response.status, "",
                        headers)


# -- rendering ------------------------------------------------------------

def _json_for_script(value):
    text = json.dumps(value, separators=(",", ":"), ensure_ascii=False)
    return (text.replace("<", "\\u003c").replace(">", "\\u003e")
            .replace("&", "\\u0026").replace("\u2028", "\\u2028")
            .replace("\u2029", "\\u2029"))


def inject(page, data, short=False):
    """The page with the rendered block and its data, or None."""
    if 'data-page="repo"' not in page or PLACEHOLDER not in page \
            or CHARSET not in page:
        return None
    payload = {k: data[k] for k in ("owner", "repo", "ref", "commit",
                                    "servedBy", "tree", "readme")}
    about = cached_about(data["owner"], data["repo"])
    if data.get("info"):
        payload["info"] = data["info"]
    if about and isinstance(about.get("features"), dict):
        payload["features"] = {key: bool(value) for key, value in about["features"].items()
                               if key in ("issues", "pulls", "discussions")}
    head = (CHARSET + '\n    <script type="application/json" '
            'id="forkmesh-repo-ssr">' + _json_for_script(payload)
            + "</script>\n    " + STYLE)
    page = page.replace(CHARSET, head, 1)
    page = page.replace(HEADER_CONTEXT, header_context(data, short), 1)
    page = page.replace(HEADER_META, HEADER_META.replace(
        '"hidden shrink-0', '"hidden sm:flex shrink-0'), 1)
    page = page.replace(HEADER_VISIBILITY, HEADER_VISIBILITY.replace(
        "></span>", ">public</span>"), 1)
    page = page.replace(HEADER_AVAILABILITY, HEADER_AVAILABILITY.replace(
        'class="inline-flex', 'class="hidden inline-flex'), 1)
    return page.replace(PLACEHOLDER, render_view(data, short, about), 1)


ISSUE_RENDER_SECONDS = 60.0


def issue_article(bundle, data, short=False):
    from . import repo_ssr_issue
    model = bundle.model or {}
    if not model.get("issue") or not model.get("issueJson"):
        return None
    key = ("dashboard", bool(short))
    now = time.time()
    kept = bundle.rendered.get(key)
    if kept and now - kept[0] < ISSUE_RENDER_SECONDS:
        return kept[1]
    owner, repo = data["owner"], data["repo"]
    link = "/" + urllib.parse.quote(repo) if short else "/%s/%s" % (
        urllib.parse.quote(owner), urllib.parse.quote(repo))
    ctx = repo_ssr_issue.Context(link, "/api/repo/%s/%s" % (
        urllib.parse.quote(owner, safe=""), urllib.parse.quote(repo, safe="")),
        str(bundle.ref or data.get("ref") or "main"))
    issue = repo_ssr_issue.parse_issue(model["issueJson"], bundle.number)
    if issue["deleted"]:
        return None
    article = repo_ssr_issue.render_issue(
        issue, ctx, owner, "%s/issues/%d" % (link, bundle.number),
        index=repo_ssr_issue.index_rows(model.get("index")), now=now)
    bundle.rendered[key] = (now, article)
    return article


def inject_issue(page, data, bundle, short=False):
    if 'data-page="repo"' not in page or PLACEHOLDER not in page \
            or CHARSET not in page or bundle is None or bundle.view != "issue":
        return None
    from . import repo_ssr_issue
    try:
        article = issue_article(bundle, data, short)
    except Exception as error:  # the bundle's own first view is still correct
        print("issue ssr skipped: %r" % (error,))
        article = None
    if article is None:
        return None
    owner, repo = data["owner"], data["repo"]
    base = "/" + urllib.parse.quote(repo) if short else "/%s/%s" % (
        urllib.parse.quote(owner), urllib.parse.quote(repo))
    tree = data.get("tree") or {}
    counts = tree.get("counts") if isinstance(tree.get("counts"), dict) else {}
    tabs = _tabs(base, cached_about(owner, repo), counts, "issues")
    meta = {"base": ("/api/repo/%s/%s" % (
        urllib.parse.quote(bundle.owner, safe=""),
        urllib.parse.quote(bundle.repo, safe=""))).lower(),
        "view": bundle.view, "number": bundle.number, "url": bundle.url()}
    page = page.replace(CHARSET, _view_reads_head(meta) + "\n    " + STYLE
                        + repo_ssr_issue.VIEWER_STYLE, 1)
    page = page.replace(HEADER_CONTEXT, header_context(data, short), 1)
    page = page.replace(HEADER_META, HEADER_META.replace(
        '"hidden shrink-0', '"hidden sm:flex shrink-0'), 1)
    page = page.replace(HEADER_VISIBILITY, HEADER_VISIBILITY.replace(
        "></span>", ">public</span>"), 1)
    page = page.replace(HEADER_AVAILABILITY, HEADER_AVAILABILITY.replace(
        'class="inline-flex', 'class="hidden inline-flex'), 1)
    return page.replace(PLACEHOLDER, (
        '<div data-repo-ssr data-repo-layout="first-view" class="min-w-0">%s%s%s</div>'
        % (_icon_sprite(), tabs, repo_ssr_issue.FRAME % article)), 1)


def _view_reads_head(meta):
    return (CHARSET + "\n    <script>window.__forkmeshView=" +
            _json_for_script(meta) + ";window.__forkmeshView.pending=fetch("
            "window.__forkmeshView.url,{headers:{accept:'application/json'}})"
            ".then(function(r){return r.ok?r.json():null})"
            ".catch(function(){return null});</script>")


def inject_view_reads(page, owner, repo, view, number):
    if 'data-page="repo"' not in page or CHARSET not in page:
        return None
    base = "/api/repo/%s/%s" % (urllib.parse.quote(owner, safe=""),
                                urllib.parse.quote(repo, safe=""))
    query = [("view", view)]
    if number:
        query.append(("number", str(number)))
    meta = {"base": base.lower(), "view": view, "number": number,
            "url": base + "/page-reads?" + urllib.parse.urlencode(query)}
    return page.replace(CHARSET, _view_reads_head(meta), 1)


def inject_view(page, bundle, section=""):
    """A pull request, issue or discussion page with its first view
    (repo_views.render) and the address of its read bundle, which a small
    head script starts fetching at once; the dashboard takes its reads from
    it (seededRepoRead). None when there is nothing to render."""
    if 'data-page="repo"' not in page or PLACEHOLDER not in page \
            or CHARSET not in page:
        return None
    from . import repo_views
    view = repo_views.render(bundle, section)
    if view is None:
        return None
    meta = {"base": ("/api/repo/%s/%s" % (
        urllib.parse.quote(bundle.owner, safe=""),
        urllib.parse.quote(bundle.repo, safe=""))).lower(),
        "view": bundle.view, "number": bundle.number, "url": bundle.url()}
    head = (_view_reads_head(meta) + "\n    " + STYLE
            + repo_views.VIEW_STYLE)
    page = page.replace(CHARSET, head, 1)
    return page.replace(PLACEHOLDER, view, 1)


def _when(value):
    text = str(value or "")
    return html.escape(text[:10])


def _parse_time(value):
    if value in (None, ""):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        number = None
    if number is not None and number > 0:
        return number / 1000 if number >= 1e12 else number
    try:
        moment = datetime.datetime.fromisoformat(
            str(value).strip().replace("Z", "+00:00"))
    except ValueError:
        return None
    if moment.tzinfo is None:
        moment = moment.replace(tzinfo=datetime.timezone.utc)
    return moment.timestamp()


TIME_UNITS = (("year", 31536000), ("month", 2592000), ("week", 604800),
              ("day", 86400), ("hour", 3600), ("minute", 60))


def time_ago(value, now=None):
    moment = _parse_time(value)
    if moment is None:
        return "unknown"
    seconds = max(0, round((time.time() if now is None else now) - moment))
    if seconds < 45:
        return "just now"
    for name, size in TIME_UNITS:
        count = seconds // size
        if count >= 1:
            return "%d %s%s ago" % (count, name, "" if count == 1 else "s")
    return "just now"


def raw_prefix(data):
    return "/api/repo/%s/%s/raw?ref=%s&path=" % (
        urllib.parse.quote(data["owner"], safe=""),
        urllib.parse.quote(data["repo"], safe=""),
        urllib.parse.quote(str(data.get("ref") or data.get("commit") or ""),
                           safe=""))


ICONS = {
    'activity': (
        '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"></path>'),
    'book-open': (
        '<path d="M12 7v14"></path><path d="M3 18a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h5a4 4 0 0 1 4 4 4 4 0 0 1 4-4h5a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1h-6a3 3 0 0 0-3 3 3 3 0 0 0-3-3z"></path>'),
    'chart-gantt': (
        '<path d="M10 6h8"></path><path d="M12 16h6"></path><path d="M3 3v16a2 2 0 0 0 2 2h16"></path><path d="M8 11h7"></path>'),
    'chart-no-axes-combined': (
        '<path d="M12 16v5"></path><path d="M16 14v7"></path><path d="M20 10v11"></path><path d="m22 3-8.646 8.646a.5.5 0 0 1-.708 0L9.354 8.354a.5.5 0 0 0-.707 0L2 15"></path><path d="M4 18v3"></path><path d="M8 14v7"></path>'),
    'chevron-down': (
        '<path d="m6 9 6 6 6-6"></path>'),
    'chevron-right': (
        '<path d="m9 18 6-6-6-6"></path>'),
    'circle-dot': (
        '<circle cx="12" cy="12" r="10"></circle><circle cx="12" cy="12" r="1"></circle>'),
    'code': (
        '<polyline points="16 18 22 12 16 6"></polyline><polyline points="8 6 2 12 8 18"></polyline>'),
    'code-2': (
        '<path d="m18 16 4-4-4-4"></path><path d="m6 8-4 4 4 4"></path><path d="m14.5 4-5 16"></path>'),
    'file': (
        '<path d="M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z"></path><path d="M14 2v4a2 2 0 0 0 2 2h4"></path>'),
    'folder': (
        '<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"></path>'),
    'git-branch': (
        '<line x1="6" x2="6" y1="3" y2="15"></line><circle cx="18" cy="6" r="3"></circle><circle cx="6" cy="18" r="3"></circle><path d="M18 9a9 9 0 0 1-9 9"></path>'),
    'git-fork': (
        '<circle cx="12" cy="18" r="3"></circle><circle cx="6" cy="6" r="3"></circle><circle cx="18" cy="6" r="3"></circle><path d="M18 9v2c0 .6-.4 1-1 1H7c-.6 0-1-.4-1-1V9"></path><path d="M12 12v3"></path>'),
    'git-pull-request': (
        '<circle cx="18" cy="18" r="3"></circle><circle cx="6" cy="6" r="3"></circle><path d="M13 6h3a2 2 0 0 1 2 2v7"></path><line x1="6" x2="6" y1="9" y2="21"></line>'),
    'history': (
        '<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"></path><path d="M3 3v5h5"></path><path d="M12 7v5l4 2"></path>'),
    'image-plus': (
        '<path d="M16 5h6"></path><path d="M19 2v6"></path><path d="M21 11.5V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h7.5"></path><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"></path><circle cx="9" cy="9" r="2"></circle>'),
    'layout-grid': (
        '<rect width="7" height="7" x="3" y="3" rx="1"></rect><rect width="7" height="7" x="14" y="3" rx="1"></rect><rect width="7" height="7" x="14" y="14" rx="1"></rect><rect width="7" height="7" x="3" y="14" rx="1"></rect>'),
    'link': (
        '<path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"></path><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"></path>'),
    'message-square': (
        '<path d="M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"></path>'),
    'plus': (
        '<path d="M5 12h14"></path><path d="M12 5v14"></path>'),
    'refresh-cw': (
        '<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"></path><path d="M21 3v5h-5"></path><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"></path><path d="M8 16H3v5"></path>'),
    'search': (
        '<circle cx="11" cy="11" r="8"></circle><path d="m21 21-4.3-4.3"></path>'),
    'star': (
        '<path d="M11.525 2.295a.53.53 0 0 1 .95 0l2.31 4.679a2.123 2.123 0 0 0 1.595 1.16l5.166.756a.53.53 0 0 1 .294.904l-3.736 3.638a2.123 2.123 0 0 0-.611 1.878l.882 5.14a.53.53 0 0 1-.771.56l-4.618-2.428a2.122 2.122 0 0 0-1.973 0L6.396 21.01a.53.53 0 0 1-.77-.56l.881-5.139a2.122 2.122 0 0 0-.611-1.879L2.16 9.795a.53.53 0 0 1 .294-.906l5.165-.755a2.122 2.122 0 0 0 1.597-1.16z"></path>'),
    'workflow': (
        '<rect width="8" height="8" x="3" y="3" rx="2"></rect><path d="M7 11v4a2 2 0 0 0 2 2h4"></path><rect width="8" height="8" x="13" y="13" rx="2"></rect>'),
}


def _icon(name, size="h-3.5 w-3.5", extra=""):
    return ('<svg class="lucide %s%s" viewBox="0 0 24 24" fill="none" '
            'stroke="currentColor" stroke-width="2" stroke-linecap="round" '
            'stroke-linejoin="round" aria-hidden="true"><use href="#fm-ssr-%s">'
            '</use></svg>' % (size, " " + extra if extra else "", name))


def _icon_sprite():
    return ('<svg width="0" height="0" style="position:absolute" '
            'aria-hidden="true">%s</svg>' % "".join(
                '<symbol id="fm-ssr-%s" viewBox="0 0 24 24">%s</symbol>' % item
                for item in sorted(ICONS.items())))


def cached_about(owner, repo):
    target = ("/api/repo/%s/%s/about" % (owner, repo)).lower()
    now = None
    for key, entry in list(_micro.items()):
        if key.lower() != target:
            continue
        if now is None:
            try:
                now = asyncio.get_running_loop().time()
            except RuntimeError:
                return None
        if now - entry[0] >= MICRO_STALE_SECONDS:
            return None
        try:
            about = json.loads(entry[3])
        except (TypeError, ValueError):
            return None
        return about if isinstance(about, dict) and about.get("ok") else None
    return None


TAB = ('<a href="%s" role="tab" aria-selected="%s" class="group relative '
       'inline-flex h-12 shrink-0 items-center px-1 text-[14px] font-normal '
       'text-foreground"><span class="inline-flex h-8 items-center gap-2 '
       'rounded-md px-2 transition-colors group-hover:bg-secondary">%s'
       '<span>%s</span>%s</span></a>')
COUNT = ('<span class="min-w-[20px] rounded-full bg-[var(--dashboard-counter)] '
         'px-1.5 text-center text-[12px] font-medium leading-[18px] '
         'text-foreground">%s</span>')
TABS = (("code", "Code", "code-2", ""), ("issues", "Issues", "circle-dot", "openIssues"),
        ("pulls", "Pull requests", "git-pull-request", "openPulls"),
        ("discussions", "Discussions", "message-square", "discussions"),
        ("insights", "Insights", "chart-no-axes-combined", ""),
        ("projects", "Projects", "chart-gantt", ""), ("actions", "Actions", "workflow", ""))
SKELETON = "rounded bg-muted-foreground/20"
ROW = ('<a href="%s" %s="%s" class="grid w-full grid-cols-[1.5rem_minmax(0,1fr)_auto] '
       'items-center gap-3 border-t border-border px-4 py-2.5 text-left text-sm '
       'hover:bg-secondary/40 transition-colors sm:grid-cols-[1.5rem_minmax(9rem,0.8fr)'
       '_minmax(0,1fr)_8rem]">%s<span class="min-w-0 truncate font-medium '
       'text-foreground">%s</span><span class="hidden w-full min-w-0 truncate '
       'text-left text-xs text-muted-foreground sm:block" style="justify-self:'
       'stretch;text-align:left">%s</span><span class="w-full shrink-0 text-right '
       'text-xs tabular-nums text-muted-foreground font-mono" style="justify-self:'
       'end;text-align:right">%s</span></a>')
ABOUT_ROW = ('<div class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 '
             'w-full rounded-md px-2 py-1.5 text-left text-xs"><span class="shrink-0 '
             'text-muted-foreground">%s</span><span class="inline-flex items-center '
             'gap-1 justify-self-end text-foreground">%s</span></div>')


def _count_label(value):
    try:
        return "{:,}".format(int(value))
    except (TypeError, ValueError):
        return ""


def _tabs(base, about, counts, selected="code"):
    features = (about or {}).get("features") or {}
    shown = []
    for key, label, icon, count_key in TABS:
        if key != "code" and about is None and selected == "code":
            continue
        if key in ("issues", "pulls", "discussions") and features.get(key) is False:
            continue
        count = ""
        if count_key and count_key in counts:
            number = counts.get(count_key)
            if key != "discussions" or (isinstance(number, int) and number > 0):
                count = COUNT % html.escape(_count_label(number))
        href = base if key == "code" else "%s/%s" % (base, key)
        shown.append(TAB % (html.escape(href), "true" if key == selected else "false",
                            _icon(icon, "h-4 w-4", "text-muted-foreground"),
                            label, count))
    return ('<div data-repo-github-header class="border-b border-border"><div class="flex w-full min-w-0 '
            'gap-1 overflow-x-auto px-3 sm:px-6" role="tablist" '
            'aria-label="Repository sections">%s</div></div>' % "".join(shown))


def _toolbar(ref):
    return (
        '<div class="grid gap-2 md:grid-cols-[auto_minmax(0,1fr)_auto_auto_auto]">'
        '<div class="flex min-w-0 flex-nowrap items-center gap-3"><div class="relative '
        'inline-flex min-w-0 max-w-64 shrink"><span class="inline-flex h-9 w-full '
        'max-w-full items-center justify-center gap-2 rounded-md border border-border '
        'bg-secondary px-3 text-xs font-medium text-foreground">%s<span class="min-w-0 '
        'truncate">%s</span>%s</span></div><span class="inline-flex h-9 items-center '
        'gap-2 whitespace-nowrap text-xs font-semibold text-muted-foreground">%s'
        '<span class="inline-block h-3 w-4 %s"></span><span>Branches</span></span></div>'
        '<span class="inline-flex h-9 min-w-0 items-center gap-2 rounded-md border '
        'border-border bg-background px-3 text-left text-xs text-muted-foreground">%s'
        '<span class="min-w-0 truncate">Go to file</span><span class="ml-auto hidden '
        'rounded border border-border px-1.5 py-0.5 font-mono text-[10px] '
        'text-muted-foreground sm:inline">T</span></span>'
        '<span class="inline-flex h-9 items-center justify-center gap-2 rounded-md border '
        'border-border bg-secondary px-3 text-xs font-semibold text-foreground">%s'
        '<span>Code map</span></span>'
        '<span class="inline-flex h-9 items-center justify-center gap-2 rounded-md border '
        'border-border bg-secondary px-3 text-xs font-semibold text-foreground">%sAdd file%s'
        '</span><div class="flex min-w-0 items-center gap-2"><span class="inline-flex h-9 '
        'items-center justify-center gap-2 rounded-md border border-foreground/10 bg-cta '
        'px-3 text-xs font-semibold text-cta-foreground">%s<span>Code</span>%s</span></div>'
        '</div>' % (
            _icon("git-branch", "h-3.5 w-3.5", "shrink-0"), html.escape(ref),
            _icon("chevron-down", "h-3 w-3", "shrink-0 text-muted-foreground"),
            _icon("git-branch"), SKELETON,
            _icon("search", "h-3.5 w-3.5", "shrink-0"),
            _icon("layout-grid", "h-3.5 w-3.5", "text-muted-foreground"),
            _icon("plus", "h-3.5 w-3.5", "text-muted-foreground"),
            _icon("chevron-down", "h-3 w-3", "text-muted-foreground"),
            _icon("code"), _icon("chevron-down", "h-3 w-3")))


def _provenance(about):
    upstream = str((about or {}).get("mirrorOf") or "").strip()
    if upstream.lower().startswith(("https://", "http://")):
        synced = _parse_time((about or {}).get("lastSyncedAt"))
        label = re.sub(r"^https?://", "", upstream).rstrip("/")
        return (
            '<span class="inline-flex min-w-0 items-center gap-1.5">%s'
            '<span class="shrink-0">Mirror of</span><a href="%s" class="dashboard-accent-link '
            'min-w-0 truncate font-medium hover:underline">%s</a>%s</span><span '
            'class="min-w-0 text-[11px] text-muted-foreground">Read-only. Send changes '
            '<a href="%s" class="dashboard-accent-link hover:underline">upstream</a> or '
            'into a fork.</span>' % (
                _icon("refresh-cw", "h-3.5 w-3.5", "shrink-0 text-muted-foreground"),
                html.escape(upstream), html.escape(label),
                ('<span class="shrink-0 text-muted-foreground">· synced %s</span>'
                 % html.escape(time_ago(synced))) if synced else "",
                html.escape(upstream)))
    parent = str((about or {}).get("forkedFrom") or "").strip().lower()
    if not re.fullmatch(r"[a-z0-9][a-z0-9._-]{0,79}/[a-z0-9._-]{1,80}", parent):
        return ""
    return (
        '<span class="inline-flex min-w-0 items-center gap-1.5">%s<span class="shrink-0">'
        'forked from</span><a href="/%s" class="dashboard-accent-link min-w-0 truncate '
        'font-medium hover:underline">%s</a></span>' % (
            _icon("git-fork", "h-3.5 w-3.5", "shrink-0 text-muted-foreground"),
            html.escape(parent), html.escape(parent)))


def _title_row(repo, base, about):
    provenance = _provenance(about)
    count = '<span class="fm-counter">–</span>'
    return (
        '<div data-repo-title-row class="mx-auto w-full max-w-7xl min-w-0 px-3 sm:px-5">'
        '<div class="flex min-w-0 flex-wrap items-center justify-between gap-3 border-b '
        'border-border py-4"><div class="flex min-w-0 items-start gap-2"><span '
        'class="mt-0.5 shrink-0"><span class="flex h-6 w-6 shrink-0 items-center '
        'justify-center overflow-hidden rounded-xl border border-border bg-secondary '
        'text-muted-foreground">%s</span></span><div class="min-w-0"><div class="flex '
        'min-w-0 flex-wrap items-center gap-2"><a href="%s" class="min-w-0 truncate '
        'text-xl font-semibold leading-7 text-foreground hover:underline">%s</a><span '
        'class="fm-label">Public</span></div>%s</div></div><div class="relative ml-auto '
        'flex shrink-0 items-center gap-2"><div class="inline-flex"><span class="fm-btn '
        'fm-btn-sm" style="border-top-right-radius:0;border-bottom-right-radius:0">%s'
        '<span>Fork</span>%s</span><span class="fm-btn fm-btn-sm" style="margin-left:-1px;'
        'padding:0 6px;border-top-left-radius:0;border-bottom-left-radius:0">%s</span></div>'
        '<span class="fm-btn fm-btn-sm">%s<span>Star</span>%s</span></div></div></div>' % (
            _icon("book-open", "h-4 w-4"), html.escape(base), html.escape(repo),
            ('<div class="mt-1 flex min-w-0 flex-wrap items-center gap-x-3 gap-y-1 '
             'text-xs text-muted-foreground">%s</div>' % provenance) if provenance else "",
            _icon("git-fork"), count, _icon("chevron-down"), _icon("star"), count))


def _about_rail(about, readme_href, info=None):
    description = str((info or {}).get("about") or (about or {}).get("description")
                      or "").strip()
    website = str((info or {}).get("website") or "").strip()
    if not website.lower().startswith(("https://", "http://")):
        website = ""
    text = (html.escape(description) if description else
            '<span class="inline-block h-3.5 w-full max-w-full align-middle %s">'
            '</span>' % SKELETON)
    dash = '<span class="font-mono">–</span>'
    site = ('<div class="grid grid-cols-[auto_minmax(0,1fr)] items-center gap-3 w-full '
            'rounded-md px-2 py-1.5 text-left text-xs"><span class="shrink-0 '
            'text-muted-foreground">Website</span><a href="%s" class="dashboard-accent-link '
            'min-w-0 truncate text-right">%s</a></div>' % (
                html.escape(website), html.escape(re.sub(r"^https?://", "", website,
                                                         flags=re.I).rstrip("/")))
            ) if website else ""
    rows = site + "".join(ABOUT_ROW % (label, value) for label, value in (
        ("Watchers", dash + _icon("chevron-down", "h-3 w-3", "text-muted-foreground")),
        ("Mirrors", dash), ("Files", dash), ("Size", dash)))
    legend = "".join('<span class="block %s">&nbsp;</span>' % SKELETON for _ in range(6))
    people = "".join('<span class="h-7 w-7 rounded-full border-2 border-background '
                     'bg-secondary"></span>' for _ in range(8))
    return (
        '<aside class="min-w-0 self-start rounded-lg border border-border bg-background p-4">'
        '<div class="flex items-center justify-between gap-3"><h3 class="text-sm '
        'font-semibold text-foreground">About</h3></div>'
        '<p class="mt-2 text-sm leading-6 text-foreground">%s</p>'
        '<div class="mt-2 flex flex-wrap gap-x-4 gap-y-1.5 text-xs text-muted-foreground">'
        '<span class="inline-flex min-w-0 items-center gap-1.5">%s<span class="min-w-0 '
        'truncate">Open clean URL</span></span><a href="%s" class="inline-flex min-w-0 '
        'items-center gap-1.5 hover:text-foreground">%s<span>Readme</span></a>'
        '<span class="inline-flex min-w-0 items-center gap-1.5">%s<span>Activity</span>'
        '</span></div><div class="-mx-2 mt-3 grid">%s</div>'
        '<div class="mt-3 border-t border-border pt-3"><h4 class="text-xs font-semibold '
        'text-foreground">Languages</h4><div class="mt-2.5 h-1.5 w-full rounded-full '
        'bg-muted-foreground/20"></div><div class="mt-2.5 grid grid-cols-2 gap-x-3 '
        'gap-y-1 text-[11px]">%s</div></div>'
        '<div class="mt-3 border-t border-border pt-3"><div class="flex items-center '
        'justify-between gap-3"><h4 class="text-xs font-semibold text-foreground">'
        'Contributors</h4><span class="font-mono text-[11px] text-muted-foreground">–'
        '</span></div><div class="mt-2.5 flex items-center -space-x-1.5">%s</div></div>'
        '<div class="mt-4 border-t border-border pt-4"><div class="flex items-center '
        'justify-between gap-2 text-xs font-semibold text-foreground"><span '
        'class="inline-flex items-center gap-1.5">%sRepository logo</span>%s</div></div>'
        '</aside>' % (
            text, _icon("link", "h-3.5 w-3.5", "shrink-0"), html.escape(readme_href),
            _icon("book-open"), _icon("activity"), rows, legend, people,
            _icon("image-plus", "h-3.5 w-3.5", "text-muted-foreground"),
            _icon("chevron-down", "h-3.5 w-3.5", "text-muted-foreground")))


def render_view(data, short=False, about=None):
    owner, repo = data["owner"], data["repo"]
    base = "/%s/%s" % (urllib.parse.quote(owner), urllib.parse.quote(repo))
    if short:
        base = "/" + urllib.parse.quote(repo)
    tree = data["tree"]
    counts = tree.get("counts") if isinstance(tree.get("counts"), dict) else {}
    latest = tree.get("latestCommit") or {}
    entries = sorted(tree.get("entries") or [], key=lambda e: (
        e.get("type") != "tree", str(e.get("name") or "").lower()))
    rows = []
    for entry in entries[:MAX_TREE_ROWS]:
        name = str(entry.get("name") or "")
        kind = "tree" if entry.get("type") == "tree" else "blob"
        href = "%s/%s/%s" % (base, kind, urllib.parse.quote(name))
        attr = "data-dashboard-tree-path" if kind == "tree" \
            else "data-dashboard-blob-path"
        message = str(entry.get("message") or entry.get("commitMessage")
                      or entry.get("subject") or "mirrored repository object")
        rows.append(ROW % (
            html.escape(href), attr, html.escape(name),
            _icon("folder" if kind == "tree" else "file", "h-4 w-4",
                  "shrink-0 text-foreground"),
            html.escape(name), html.escape(message),
            html.escape(time_ago(entry.get("date") or entry.get("updatedAt")
                                 or entry.get("committedAt")))))
    author = str(latest.get("author") or latest.get("committer") or owner)
    summary = (
        '<div class="grid gap-2 border-b border-border bg-secondary/50 px-4 py-3 text-xs '
        'sm:grid-cols-[minmax(0,1fr)_auto_auto_auto] sm:items-center"><div class="flex '
        'min-w-0 items-center gap-2"><span class="flex h-6 w-6 shrink-0 items-center '
        'justify-center rounded-full border border-border/30 bg-primary/10 font-mono '
        'text-[10px] font-semibold text-muted-foreground">%s</span><span class="min-w-0 '
        'truncate text-foreground font-medium">%s</span><span class="min-w-0 truncate '
        'text-muted-foreground">%s</span></div><span class="font-mono '
        'text-muted-foreground">%s</span><span class="font-mono text-muted-foreground">'
        '%s</span><a href="%s/commits" class="inline-flex items-center gap-1 font-medium '
        'text-foreground transition-colors hover:text-accent">%s<span>History</span>'
        '<span class="inline-block w-10 rounded-full bg-background px-1.5 py-0.5 font-mono text-[10px]">&nbsp;</span></a>'
        '</div>' % (
            html.escape((author[:1] or "F").upper()), html.escape(author),
            html.escape(str(latest.get("subject") or latest.get("message")
                            or "published latest mirror metadata")),
            html.escape(str(latest.get("hash") or data.get("commit") or "")[:7]
                        or "live"),
            html.escape(time_ago(latest.get("date"))),
            html.escape(base), _icon("history", "h-3.5 w-3.5", "text-muted-foreground")))
    readme = ""
    readme_href = base
    if data.get("readme"):
        blob = data["readme"]["data"]
        name = data["readme"]["name"]
        readme_href = "%s/blob/%s" % (base, urllib.parse.quote(name))
        text = str(blob.get("content") or "")[:MAX_README_CHARS] \
            if blob.get("encoding") != "base64" else ""
        body = ("<pre>%s</pre>" % html.escape(text)
                if name.lower().endswith(".txt")
                else markdown_html(text, base, raw_prefix(data)))
        readme = (
            '<section class="mt-4 overflow-hidden rounded-lg border border-border '
            'bg-background"><div class="flex items-center gap-2 border-b border-border '
            'bg-secondary/50 px-4 py-3 text-xs font-medium text-foreground">%s%s</div>'
            '<div class="fm-ssr-readme p-4 text-sm text-foreground" '
            'data-repo-readme-body>%s</div></section>' % (
                _icon("book-open", "h-3.5 w-3.5", "text-muted-foreground"),
                html.escape(name), body))
    return (
        '<div data-repo-ssr data-repo-layout="first-view" class="min-w-0">%s%s%s<div data-repo-content-grid class="mx-auto grid w-full max-w-7xl '
        'min-w-0 gap-5 px-3 pb-3 pt-4 sm:p-5 lg:grid-cols-[minmax(0,1fr)_18rem]">'
        '<div class="min-w-0">%s<div class="my-3 flex min-w-0 flex-col gap-2 md:flex-row '
        'md:items-center md:justify-between"><div class="flex min-w-0 items-center gap-2">'
        '<div class="min-w-0 truncate text-xs text-muted-foreground"><a href="%s" '
        'class="inline-block font-semibold text-accent hover:underline" style="line-height:1.5">%s</a></div></div></div>'
        '<div class="overflow-hidden rounded-lg border border-border bg-background">'
        '%s<div data-repo-tree>%s</div></div>%s</div>%s</div></div>' % (
            _icon_sprite(), _tabs(base, about, counts), _title_row(repo, base, about),
            _toolbar(str(data.get("ref") or "main")), html.escape(base), html.escape(repo),
            summary, "".join(rows), readme,
            _about_rail(about, readme_href, data.get("info"))))


HEADER_CONTEXT = '<span data-dashboard-header-context class="min-w-0 truncate"></span>'
HEADER_META = '<div data-repo-header-meta class="hidden shrink-0 items-center gap-1.5">'
HEADER_VISIBILITY = '<span data-repo-header-visibility class="fm-label"></span>'
HEADER_AVAILABILITY = ('<span data-repo-availability-status class="inline-flex h-5 items-center '
                       'rounded-full border border-border px-2 text-xs font-medium '
                       'text-muted-foreground"></span>')


def header_context(data, short=False):
    owner, repo = data["owner"], data["repo"]
    base = "/" + urllib.parse.quote(repo) if short else "/%s/%s" % (
        urllib.parse.quote(owner), urllib.parse.quote(repo))
    link = '<a href="%s" class="hover:underline">%s</a>' % (
        html.escape(base), html.escape(repo))
    if not short:
        link = ('<a href="/@%s" class="hover:underline">%s</a><span class="px-1 '
                'font-normal text-muted-foreground" aria-hidden="true">/</span>%s' % (
                    html.escape(urllib.parse.quote(owner)), html.escape(owner), link))
    return HEADER_CONTEXT.replace("></span>", ">%s</span>" % link)


# -- a small, escaping Markdown subset --------------------------------------

_INLINE_CODE = re.compile(r"`([^`]+)`")
_CODE_SPAN = re.compile(r"(`+[^`]*`+)")
_IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]*)\)")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
_BOLD = re.compile(r"\*\*(.+?)\*\*|__(.+?)__")
_ITALIC = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])")
_COMMENT = re.compile(r"<!--.*?-->", re.S)
_IMG_TAG = re.compile(r"<img\b[^>]*>", re.I)
_A_TAG = re.compile(r"<a\b([^>]*)>(.*?)</a\s*>", re.I | re.S)
_BLOCK_TAG = re.compile(
    r"</?(?:p|div|center|section|summary|details|ul|ol|li|table|tbody|thead|tr"
    r"|blockquote|picture|h[1-6])\b[^>]*>", re.I)
_ANY_TAG = re.compile(r"</?[a-zA-Z][a-zA-Z0-9-]*(?:\s[^>]*)?/?>")
_SIZE_HINT = re.compile(r"#w=(\d{1,4})(?:x(\d{1,4}))?$")
_RAW_BLOCK = re.compile(r"^\s*<[a-zA-Z!/]")
_CENTRED = re.compile(r"<center\b|\balign\s*=\s*[\"']?center", re.I)
IMAGE_CLASS = "my-2 inline-block max-w-full rounded-md border border-border"


def _attr(tag, name):
    match = re.search(r"(?<![\w-])%s\s*=\s*(?:\"([^\"]*)\"|'([^']*)'|([^\s>]+))"
                      % name, tag, re.I)
    if not match:
        return ""
    return match.group(1) if match.group(1) is not None else (
        match.group(2) if match.group(2) is not None else match.group(3))


def _markdown_url(url):
    return url.strip().replace(" ", "%20").replace(")", "%29")


def _flatten_html(text, block=True):
    def image(match):
        tag = match.group(0)
        source = _markdown_url(html.unescape(_attr(tag, "src")))
        if not source:
            return ""
        hint = ""
        width, height = _attr(tag, "width"), _attr(tag, "height")
        if width.isdigit() and 0 < int(width) < 10000 and "#" not in source:
            hint = "#w=%d" % int(width)
            if height.isdigit() and 0 < int(height) < 10000:
                hint += "x%d" % int(height)
        alt = re.sub(r"[\[\]]", "", html.unescape(_attr(tag, "alt")))
        return "![%s](%s%s)" % (alt, source, hint)

    def link(match):
        label = match.group(2).strip()
        href = _markdown_url(html.unescape(_attr(match.group(1), "href")))
        if not label:
            return ""
        return "[%s](%s)" % (label, href) if href else label

    text = _COMMENT.sub("", text)
    text = _IMG_TAG.sub(image, text)
    text = _A_TAG.sub(link, text)
    text = re.sub(r"</?(?:strong|b)\s*>", "**", text, flags=re.I)
    text = re.sub(r"</?(?:em|i)\s*>", "*", text, flags=re.I)
    text = re.sub(r"</?(?:code|tt)\s*>", "`", text, flags=re.I)
    text = re.sub(r"<br\s*/?>", "\n" if block else " ", text, flags=re.I)
    if block:
        text = re.sub(r"<hr\s*/?>", "\n\n---\n\n", text, flags=re.I)
        text = re.sub(r"<h([1-6])\b[^>]*>",
                      lambda m: "\n\n%s " % ("#" * int(m.group(1))), text,
                      flags=re.I)
        text = _BLOCK_TAG.sub("\n\n", text)
    text = _ANY_TAG.sub("", text)
    return html.unescape(text)


def _flatten_inline(text):
    return "".join(part if index % 2 else _flatten_html(part, block=False)
                   for index, part in enumerate(_CODE_SPAN.split(text)))


def _safe_href(url, base):
    url = url.strip()
    lowered = url.lower()
    if lowered.startswith(("http://", "https://", "mailto:")):
        return url
    if re.match(r"^[a-z][a-z0-9+.-]*:", lowered) or url.startswith("//"):
        return ""
    if url.startswith("#"):
        return url
    if url.startswith("/"):
        return url
    return "%s/blob/%s" % (base, url.lstrip("./"))


def _image_src(url, raw):
    lowered = url.lower()
    if lowered.startswith(("https://", "http://")):
        return url
    if re.match(r"^[a-z][a-z0-9+.-]*:", lowered) or url.startswith(("//", "#")):
        return ""
    if url.startswith("/"):
        return url
    if not raw:
        return ""
    parts = []
    for part in url.split("?", 1)[0].split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(urllib.parse.unquote(part))
    return raw + urllib.parse.quote("/".join(parts), safe="") if parts else ""


def _image_html(alt, url, raw):
    hint = _SIZE_HINT.search(url)
    target = url[:hint.start()] if hint else url
    source = _image_src(target, raw)
    if not source:
        return html.escape(alt)
    size = ""
    if hint:
        size = ' width="%s"' % hint.group(1)
        if hint.group(2):
            size += ' height="%s"' % hint.group(2)
    external = ' referrerpolicy="no-referrer"' \
        if source.lower().startswith("http") else ""
    return ('<img src="%s" alt="%s"%s%s class="%s" loading="lazy" '
            'decoding="async">' % (html.escape(source), html.escape(alt), size,
                                   external, IMAGE_CLASS))


def _inline(text, base, raw=""):
    codes = []

    def keep_code(match):
        codes.append("<code>%s</code>" % html.escape(match.group(1)))
        return "\x00%d\x00" % (len(codes) - 1)

    text = _INLINE_CODE.sub(keep_code, _flatten_inline(text))
    images = []

    def keep_image(match):
        images.append(_image_html(match.group(1), match.group(2), raw))
        return "\x01%d\x01" % (len(images) - 1)

    text = _IMAGE.sub(keep_image, text)
    links = []

    def keep_link(match):
        href = _safe_href(match.group(2), base)
        label = html.escape(match.group(1))
        links.append('<a href="%s">%s</a>' % (html.escape(href), label)
                     if href else label)
        return "\x03%d\x03" % (len(links) - 1)

    text = _LINK.sub(keep_link, text)
    text = html.escape(text, quote=False)
    from .repo_ssr_issue import MD_BARE_URL, MD_MENTION, mention_html

    def keep_markup(markup):
        links.append(markup)
        return "\x03%d\x03" % (len(links) - 1)

    text = MD_BARE_URL.sub(lambda m: m.group(1) + keep_markup(m.group(2)), text)
    text = MD_MENTION.sub(lambda m: m.group(1) + keep_markup(mention_html(m.group(2))),
                          text)
    text = _BOLD.sub(lambda m: "<strong>%s</strong>" % (m.group(1) or m.group(2)),
                     text)
    text = _ITALIC.sub(lambda m: "<em>%s</em>" % m.group(1), text)
    text = re.sub("\x03(\\d+)\x03", lambda m: links[int(m.group(1))], text)
    text = re.sub("\x01(\\d+)\x01", lambda m: images[int(m.group(1))], text)
    return re.sub("\x00(\\d+)\x00", lambda m: codes[int(m.group(1))], text)


def markdown_html(source, base, raw="", depth=0):
    out = []
    source = re.sub("[\x00-\x03]", "", source)
    lines = source.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    paragraph = []
    lists = None  # ("ul"|"ol", [items])
    i = 0

    def flush():
        nonlocal paragraph, lists
        if paragraph:
            out.append("<p>%s</p>" % _inline(" ".join(paragraph), base, raw))
            paragraph = []
        if lists:
            tag, items = lists
            out.append("<%s>%s</%s>" % (tag, "".join(
                "<li>%s</li>" % _inline(item, base, raw) for item in items), tag))
            lists = None

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        fence = re.match(r"^(```|~~~)", stripped)
        if fence:
            flush()
            marker = fence.group(1)
            block = []
            i += 1
            while i < len(lines) and not lines[i].strip().startswith(marker):
                block.append(lines[i])
                i += 1
            out.append("<pre><code>%s</code></pre>" % html.escape(
                "\n".join(block)))
            i += 1
            continue
        if depth < 2 and _RAW_BLOCK.match(line):
            flush()
            block = []
            while i < len(lines) and lines[i].strip():
                block.append(lines[i])
                i += 1
            text = "\n".join(block)
            rendered = markdown_html(_flatten_html(text), base, raw, depth + 1)
            out.append('<div class="text-center">%s</div>' % rendered
                       if _CENTRED.search(text) else rendered)
            continue
        heading = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", stripped)
        if heading:
            flush()
            level = len(heading.group(1))
            out.append("<h%d>%s</h%d>" % (level, _inline(heading.group(2),
                                                        base, raw), level))
        elif not stripped or re.match(r"^(-{3,}|\*{3,}|_{3,})$", stripped):
            flush()
            if stripped:
                out.append("<hr>")
        elif re.match(r"^\s*[-*+]\s+", line) or re.match(r"^\s*\d+[.)]\s+",
                                                         line):
            ordered = bool(re.match(r"^\s*\d+[.)]\s+", line))
            tag = "ol" if ordered else "ul"
            if paragraph:
                out.append("<p>%s</p>" % _inline(" ".join(paragraph), base, raw))
                paragraph = []
            if lists and lists[0] != tag:
                flush()
            if not lists:
                lists = (tag, [])
            lists[1].append(re.sub(r"^\s*(?:[-*+]|\d+[.)])\s+", "", line))
        elif stripped.startswith(">"):
            flush()
            quote = []
            while i < len(lines) and lines[i].strip().startswith(">"):
                quote.append(lines[i].strip()[1:].strip())
                i += 1
            out.append("<blockquote><p>%s</p></blockquote>" % _inline(
                " ".join(quote), base, raw))
            continue
        elif lists and line.startswith(("  ", "\t")):
            lists[1][-1] += " " + stripped
        else:
            if lists:
                flush()
            paragraph.append(stripped)
        i += 1
    flush()
    return "".join(out)
