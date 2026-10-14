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

The reads start when the request arrives, in parallel with the Worker, and
are skipped (the page is served as before) when they take longer than
``SSR_BUDGET_SECONDS`` or the gateway does not serve the repository publicly.
A page carrying rendered data is sent ``Cache-Control: no-cache``.
"""

from __future__ import annotations

import asyncio
import html
import json
import re
import urllib.parse

PAGE_RE = re.compile(r"^/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})/?$")
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
PLACEHOLDER = ('<p class="text-sm text-muted-foreground">'
               'Choose a repository to inspect it.</p>')
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
        node = await fastpath.node_name() or ""
        return {"owner": owner, "repo": repo, "ref": ref, "commit": commit,
                "servedBy": node, "tree": tree, "readme": readme}
    except Exception:
        return None


class ViewTicket:
    """A pull request, issue or discussion page being rendered
    (repo_views): the bundle build and the tab it opens on."""

    def __init__(self, task, section, target=None):
        self.task = task
        self.section = section
        self.target = target


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
        return ViewTicket(task, section, (owner, repo, name, number))
    target = page_target(scope)
    if target is None:
        return None
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
        try:
            data = await asyncio.wait_for(asyncio.shield(task),
                                          SSR_BUDGET_SECONDS)
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


# -- rendering ------------------------------------------------------------

def _json_for_script(value):
    text = json.dumps(value, separators=(",", ":"), ensure_ascii=False)
    return (text.replace("<", "\\u003c").replace(">", "\\u003e")
            .replace("&", "\\u0026").replace("\u2028", "\\u2028")
            .replace("\u2029", "\\u2029"))


def inject(page, data):
    """The page with the rendered block and its data, or None."""
    if 'data-page="repo"' not in page or PLACEHOLDER not in page \
            or CHARSET not in page:
        return None
    payload = {k: data[k] for k in ("owner", "repo", "ref", "commit",
                                    "servedBy", "tree", "readme")}
    head = (CHARSET + '\n    <script type="application/json" '
            'id="forkmesh-repo-ssr">' + _json_for_script(payload)
            + "</script>\n    " + STYLE)
    page = page.replace(CHARSET, head, 1)
    return page.replace(PLACEHOLDER, render_view(data), 1)


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


def render_view(data):
    owner, repo = data["owner"], data["repo"]
    base = "/%s/%s" % (urllib.parse.quote(owner), urllib.parse.quote(repo))
    tree = data["tree"]
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
        rows.append(
            '<div class="fm-ssr-row"><a href="%s" %s="%s">%s%s</a>'
            '<span class="m">%s</span><time>%s</time></div>' % (
                html.escape(href), attr, html.escape(name),
                html.escape(name), "/" if kind == "tree" else "",
                html.escape(str(entry.get("subject") or "")),
                _when(entry.get("date"))))
    commit_line = ""
    if latest:
        commit_line = (
            '<div class="fm-ssr-commit"><b>%s</b><span>%s</span>'
            '<span style="margin-left:auto;flex-shrink:0">%s &middot; %s'
            '</span></div>' % (
                html.escape(str(latest.get("author") or "")),
                html.escape(str(latest.get("subject") or "")),
                html.escape(str(latest.get("hash") or "")[:7]),
                _when(latest.get("date"))))
    readme = ""
    if data.get("readme"):
        blob = data["readme"]["data"]
        name = data["readme"]["name"]
        text = str(blob.get("content") or "")[:MAX_README_CHARS] \
            if blob.get("encoding") != "base64" else ""
        body = ("<pre>%s</pre>" % html.escape(text)
                if name.lower().endswith(".txt")
                else markdown_html(text, base))
        readme = ('<div class="fm-ssr-box fm-ssr-readme">'
                  '<h2 class="fm-ssr-file">%s</h2>'
                  '<div class="fm-ssr-md" data-repo-readme-body>%s</div>'
                  '</div>' % (html.escape(name), body))
    return (
        '<div class="fm-ssr" data-repo-ssr>'
        '<h1 class="fm-ssr-title"><a href="/%s">%s</a> / '
        '<a href="%s">%s</a></h1>'
        '<div class="fm-ssr-box">%s<div data-repo-tree>%s</div></div>%s'
        '</div>' % (
            html.escape(urllib.parse.quote(owner)), html.escape(owner),
            html.escape(base), html.escape(repo), commit_line,
            "".join(rows), readme))


# -- a small, escaping Markdown subset --------------------------------------
# Enough for a README to read well until the dashboard's own renderer takes
# over: headings, paragraphs, lists, quotes, fenced code, inline code,
# emphasis and links. Raw HTML is shown as text (never interpreted), images
# become their alt text, and only http(s), mailto and relative links are
# kept (relative ones resolve inside the repository, like the dashboard).

_INLINE_CODE = re.compile(r"`([^`]+)`")
_IMAGE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
_LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
_BOLD = re.compile(r"\*\*(.+?)\*\*|__(.+?)__")
_ITALIC = re.compile(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])")
_TAG = re.compile(r"<[^>\n]{1,400}>")


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


def _inline(text, base):
    codes = []

    def keep_code(match):
        codes.append("<code>%s</code>" % html.escape(match.group(1)))
        return "\x00%d\x00" % (len(codes) - 1)

    text = _INLINE_CODE.sub(keep_code, text)
    text = _TAG.sub("", text)
    text = _IMAGE.sub(lambda m: "\x01%s\x02" % m.group(1), text)
    links = []

    def keep_link(match):
        href = _safe_href(match.group(2), base)
        label = html.escape(match.group(1))
        links.append('<a href="%s">%s</a>' % (html.escape(href), label)
                     if href else label)
        return "\x03%d\x03" % (len(links) - 1)

    text = _LINK.sub(keep_link, text)
    text = html.escape(text, quote=False)
    text = _BOLD.sub(lambda m: "<strong>%s</strong>" % (m.group(1) or m.group(2)),
                     text)
    text = _ITALIC.sub(lambda m: "<em>%s</em>" % m.group(1), text)
    text = text.replace("\x01", "").replace("\x02", "")
    text = re.sub("\x03(\\d+)\x03", lambda m: links[int(m.group(1))], text)
    return re.sub("\x00(\\d+)\x00", lambda m: codes[int(m.group(1))], text)


def markdown_html(source, base):
    out = []
    lines = source.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    paragraph = []
    lists = None  # ("ul"|"ol", [items])
    i = 0

    def flush():
        nonlocal paragraph, lists
        if paragraph:
            out.append("<p>%s</p>" % _inline(" ".join(paragraph), base))
            paragraph = []
        if lists:
            tag, items = lists
            out.append("<%s>%s</%s>" % (tag, "".join(
                "<li>%s</li>" % _inline(item, base) for item in items), tag))
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
        heading = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", stripped)
        if heading:
            flush()
            level = len(heading.group(1))
            out.append("<h%d>%s</h%d>" % (level, _inline(heading.group(2),
                                                        base), level))
        elif not stripped or re.match(r"^(-{3,}|\*{3,}|_{3,})$", stripped):
            flush()
            if stripped:
                out.append("<hr>")
        elif re.match(r"^\s*[-*+]\s+", line) or re.match(r"^\s*\d+[.)]\s+",
                                                         line):
            ordered = bool(re.match(r"^\s*\d+[.)]\s+", line))
            tag = "ol" if ordered else "ul"
            if paragraph:
                out.append("<p>%s</p>" % _inline(" ".join(paragraph), base))
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
                " ".join(quote), base))
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
