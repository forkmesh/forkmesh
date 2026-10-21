"""Node-local fast path for public repository reads.

On Cloudflare every ``/api/repo/<owner>/<repo>/<op>`` read (tree, blob, raw,
history, ...) crossed the public internet to a remote mirror through a
router-signed capability, with mirror selection, signed health proofs and an
edge metadata cache in front. On a Next core node the same bare repository is
served by the node's own Git gateway (tools/mirror_gateway.py on
127.0.0.1:8790), so this module answers those reads straight from it:

* only the gateway's unauthenticated ``/v1/public/repositories/...`` route is
  used. The gateway serves it for repositories its signed configuration lists
  as public and enabled, and nothing else; private, unknown, quarantined and
  disabled repositories answer 404 there;
* any answer other than 200 (404, 503, a refused connection, a timeout) falls
  through to the Worker, which still owns the canonical answer, aliases,
  private views and outage fallbacks. A request carrying a private-view token
  (``viewer``/``ts``/``sig``) never takes the fast path;
* the query sent to the gateway is exactly the one the Worker's router builds
  (``_https_mirror_request_query`` in app/src/entry.py), and the body is passed
  through untouched, so the JSON is byte-identical to the routed read;
* caching: a read pinned to a full commit id is immutable
  (``public, max-age=31536000, immutable``); anything else is ``no-cache``
  with a strong ETag, so a revisit costs a 304 instead of the bytes;
* a read without ``ref`` (the default branch) reports the branch it resolved
  in ``X-ForkMesh-Ref``, which lets the repository page's early fetch
  (partials/views/repo.html) be matched to the dashboard's later ``ref=main``
  request. ``X-ForkMesh-Local-Reads: 1`` tells the page that the canonical
  origin reads locally, so it need not open connections to mirror origins.

Configuration: ``FORKMESH_HOST_REPO_GATEWAY`` (a loopback base URL, or
``off``). Unset, it defaults to ``http://127.0.0.1:8790`` on a fleet node
(``FORKMESH_NODE_ID`` set) and is off elsewhere.
"""

from __future__ import annotations

import asyncio
import collections
import hashlib
import json
import os
import re
import time
import urllib.parse

API_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/([^/]+)$")
READ_OPERATIONS = frozenset({
    "tree", "blob", "blobs", "raw", "history", "commit", "compare",
    "merge-preview", "branches", "search", "stats", "sizes", "pulse",
})
# entry.HTTPS_MIRROR_REF_OPERATIONS
REF_OPERATIONS = frozenset({
    "tree", "blobs", "blob", "raw", "history", "search", "stats", "sizes",
})
MAX_BLOB_BATCH = 60  # entry.MAX_BLOB_BATCH
PRIVATE_VIEW_FIELDS = ("viewer", "ts", "sig")
OWNER_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
FULL_COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
IMMUTABLE = "public, max-age=31536000, immutable"
REVALIDATE = "no-cache"
# Raw bodies up to this size are buffered so they get an ETag (repository
# logos and banners on every page view); larger ones stream, uncached.
RAW_BUFFER_MAX = 4 * 1024 * 1024
READ_TIMEOUT_SECONDS = 20.0
PASS_HEADERS = ("content-type", "content-disposition", "content-length")
# Commit-pinned answers kept in host memory (bytes of body), so a pull's diff
# (compare?base=<sha>&head=<sha>) and the pull ledger's blobs survive the
# gateway's per-push cache generation, and a page's reads are assembled
# without a gateway round trip each (repo_views).
IMMUTABLE_CACHE_BYTES = 48 * 1024 * 1024
IMMUTABLE_CACHE_ENTRY_MAX = 4 * 1024 * 1024
# The host-side view bundles (repo_views): the pull/issue/discussion reads a
# page needs, in one response.
VIEW_OPERATION = "page-reads"


def repository_read_path(value):
    """entry._repository_read_path: one spelling per directory."""
    return "/".join(
        segment for segment in str(value or "").split("/")
        if segment not in ("", "."))


def gateway_query(operation, query_string):
    """The query the Worker's router sends for this read, as pairs.

    Mirrors entry._https_mirror_request_query for READ_OPERATIONS; returns
    None when the request must stay on the Worker (private-view tokens)."""
    params = urllib.parse.parse_qs(query_string or "", keep_blank_values=True,
                                   max_num_fields=200)
    if any(value for name in PRIVATE_VIEW_FIELDS
           for value in params.get(name, [])):
        return None
    out = []
    if operation == "blobs":
        for path in params.get("path", [])[:MAX_BLOB_BATCH]:
            out.append(("path", repository_read_path(path)))
    elif operation == "search":
        query = params.get("q", params.get("path", [""]))[0]
        if query:
            out.append(("path", query))
    elif operation in ("compare", "merge-preview"):
        for key in ("base", "head"):
            if params.get(key):
                out.append((key, params[key][0]))
    elif operation in ("tree", "blob", "raw", "commit", "history"):
        if params.get("path"):
            out.append(("path", repository_read_path(params["path"][0])))
    elif operation == "pulse":
        for key in ("period", "person", "view"):
            if params.get(key):
                out.append((key, params[key][0]))
    if params.get("ref") and operation in REF_OPERATIONS:
        out.append(("ref", params["ref"][0]))
    return out


def cache_control(operation, pairs):
    values = dict(pairs)
    ref = values.get("ref", "")
    if FULL_COMMIT_RE.match(ref.lower()) and operation in REF_OPERATIONS:
        return IMMUTABLE
    # A diff between two commit ids never changes.
    if operation in ("compare", "merge-preview") and all(
            FULL_COMMIT_RE.match(values.get(key, "").lower())
            for key in ("base", "head")):
        return IMMUTABLE
    return REVALIDATE


SIZES_DEPTH_MAX = 8
_trimmed_sizes = {}


def trim_sizes(body, query_string):
    """/sizes cut to ``depth`` levels below the root (``?depth=N``).

    The About rail's size chart draws three rings from a tree that is
    ~400 KB of JSON for forkmesh/forkmesh; three levels are ~95 KB. Without
    ``depth`` (the Size map tab) the whole tree is sent."""
    params = urllib.parse.parse_qs(
        query_string.decode("latin-1") if isinstance(query_string, bytes)
        else query_string, max_num_fields=50)
    text = (params.get("depth") or [""])[0]
    if not text.isdigit() or not 0 < int(text) <= SIZES_DEPTH_MAX:
        return body
    depth = int(text)
    key = (hashlib.sha256(body).digest(), depth)
    cached = _trimmed_sizes.get(key)
    if cached is not None:
        return cached
    try:
        tree = json.loads(body)
    except ValueError:
        return body

    def cut(node, level):
        if not isinstance(node, dict):
            return node
        out = {k: v for k, v in node.items() if k != "children"}
        children = node.get("children")
        if isinstance(children, list) and level < depth:
            out["children"] = [cut(child, level + 1) for child in children]
        elif isinstance(children, list) and children:
            out["children"] = []
        return out
    trimmed = json.dumps(cut(tree, 0), separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8")
    if len(_trimmed_sizes) > 32:
        _trimmed_sizes.clear()
    _trimmed_sizes[key] = trimmed
    return trimmed


class ImmutableCache:
    """Least-recently-used answers to commit-pinned reads, bounded by bytes."""

    def __init__(self, max_bytes=IMMUTABLE_CACHE_BYTES):
        self.max_bytes = max_bytes
        self.bytes = 0
        self._entries = collections.OrderedDict()
        self.hits = 0
        self.misses = 0

    def get(self, key):
        entry = self._entries.get(key)
        if entry is None:
            self.misses += 1
            return None
        self._entries.move_to_end(key)
        self.hits += 1
        return entry

    def put(self, key, content_type, body):
        if len(body) > IMMUTABLE_CACHE_ENTRY_MAX or key in self._entries:
            return
        self._entries[key] = [content_type, body, None]
        self.bytes += len(body)
        while self.bytes > self.max_bytes and self._entries:
            _key, old = self._entries.popitem(last=False)
            self.bytes -= len(old[1])


def etag_for(body):
    return '"fm-%s"' % hashlib.sha256(body).hexdigest()[:32]


def _normal_tag(tag):
    # A compressing proxy may weaken (W/) or suffix ("-zstd") the tag it
    # passes on; the browser echoes that spelling back.
    tag = tag.strip()
    if tag.startswith("W/"):
        tag = tag[2:]
    return re.sub(r'-(?:zstd|gzip|br)"$', '"', tag)


def _if_none_match(headers, etag):
    for name, value in headers:
        if name == b"if-none-match":
            tags = [_normal_tag(t)
                    for t in value.decode("latin-1").split(",")]
            return etag in tags or "*" in tags
    return False


def configured_base_url(environ=None):
    environ = os.environ if environ is None else environ
    value = environ.get("FORKMESH_HOST_REPO_GATEWAY")
    if value is None:
        return "http://127.0.0.1:8790" if environ.get("FORKMESH_NODE_ID") \
            else ""
    value = value.strip()
    if value.lower() in ("", "0", "off", "none", "false"):
        return ""
    return value.rstrip("/")


def allowed_origins(environ=None):
    environ = os.environ if environ is None else environ
    value = (environ.get("FORKMESH_VAR_CORS_ALLOWED_ORIGINS")
             or environ.get("CORS_ALLOWED_ORIGINS") or "")
    return {o.strip().rstrip("/") for o in value.split(",") if o.strip()}


def with_cors(scope, send, environ=None):
    origin = ""
    for name, value in scope.get("headers") or []:
        if name == b"origin":
            origin = value.decode("latin-1").strip()
    allowed = origin and origin in allowed_origins(environ)

    async def wrapped(message):
        if message.get("type") == "http.response.start":
            headers = list(message.get("headers") or [])
            headers.append((b"vary", b"origin"))
            if allowed:
                headers += [(b"access-control-allow-origin", origin.encode("latin-1")),
                            (b"access-control-expose-headers",
                             b"etag, x-forkmesh-ref, x-forkmesh-served-by")]
            message = dict(message, headers=headers)
        await send(message)
    return wrapped


class RepoFastPath:
    """Answers public repository reads from the node-local Git gateway."""

    def __init__(self, base_url, *, client=None):
        self.base_url = base_url.rstrip("/")
        self._client = client
        self._node = None
        self._node_checked = 0.0
        self._default_ref = {}
        self.immutable = ImmutableCache()
        self._inflight = {}
        self.stats = {"served": 0, "fallthrough": 0, "not_modified": 0,
                      "memory": 0}

    @classmethod
    def from_environ(cls, environ=None):
        base = configured_base_url(environ)
        return cls(base) if base else None

    # -- gateway client ------------------------------------------------------
    def client(self):
        if self._client is None:
            import httpx
            self._client = httpx.AsyncClient(
                base_url=self.base_url,
                timeout=httpx.Timeout(READ_TIMEOUT_SECONDS, connect=0.5),
                limits=httpx.Limits(max_connections=16,
                                    max_keepalive_connections=8),
                trust_env=False)
        return self._client

    async def aclose(self):
        if self._client is not None:
            await self._client.aclose()

    async def node_name(self):
        """The gateway's node name, for X-ForkMesh-Served-By (like the
        Worker's routed reads); refreshed every 5 minutes."""
        now = time.monotonic()
        if self._node is not None and now - self._node_checked < 300:
            return self._node
        self._node_checked = now
        try:
            response = await self.client().get("/health")
            if response.status_code == 200:
                name = str(response.json().get("node") or "")
                if re.fullmatch(r"[A-Za-z0-9._-]{1,64}", name):
                    self._node = name
        except Exception:
            pass
        return self._node

    async def default_branch(self, owner, repo, commit):
        """Name of the branch a ref-less read resolved (the gateway's HEAD).

        Keyed by the resolved commit, so a push that moves HEAD is re-read."""
        key = (owner, repo)
        cached = self._default_ref.get(key)
        if cached and cached[0] == commit:
            return cached[1]
        try:
            response = await self.client().get(
                "/v1/public/repositories/%s/%s/branches" % (
                    urllib.parse.quote(owner, safe=""),
                    urllib.parse.quote(repo, safe="")))
            if response.status_code != 200:
                return ""
            data = response.json()
        except Exception:
            return ""
        name = str(data.get("defaultBranch") or "")
        head = ""
        for branch in data.get("branches") or []:
            if isinstance(branch, dict) and branch.get("name") == name:
                head = str(branch.get("commit") or branch.get("sha") or "")
        if head and commit and head.lower() != commit.lower():
            # HEAD moved between the two reads; do not claim a name.
            return ""
        if len(self._default_ref) > 256:
            self._default_ref.clear()
        self._default_ref[key] = (commit, name)
        return name

    async def read(self, url, immutable=False):
        """(status, body, parsed JSON or None) of one gateway GET.

        Concurrent reads of one URL share a request; a commit-pinned 200 is
        answered from host memory afterwards."""
        if immutable:
            entry = self.immutable.get(url)
            if entry is not None:
                if entry[2] is None:
                    entry[2] = json.loads(entry[1])
                return 200, entry[1], entry[2]
        pending = self._inflight.get(url)
        if pending is None:
            pending = asyncio.ensure_future(self._read(url, immutable))
            self._inflight[url] = pending
            pending.add_done_callback(
                lambda _f, url=url: self._inflight.pop(url, None))
        return await asyncio.shield(pending)

    async def _read(self, url, immutable):
        try:
            response = await self.client().get(url)
        except Exception:
            return 0, b"", None
        body = response.content
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = None
        if immutable and response.status_code == 200 and parsed is not None:
            self.immutable.put(url, response.headers.get(
                "content-type", "application/json"), body)
            entry = self.immutable.get(url)
            if entry is not None:
                entry[2] = parsed
        return response.status_code, body, parsed

    @staticmethod
    def gateway_url(owner, repo, operation, pairs):
        url = "/v1/public/repositories/%s/%s/%s" % (
            urllib.parse.quote(owner, safe=""),
            urllib.parse.quote(repo, safe=""),
            urllib.parse.quote(operation, safe=""))
        if pairs:
            url += "?" + urllib.parse.urlencode(pairs)
        return url

    # -- ASGI --------------------------------------------------------------
    @staticmethod
    def match(scope):
        """(owner, repo, operation) for a fast-path candidate, else None."""
        if scope.get("method", "GET").upper() not in ("GET", "HEAD"):
            return None
        raw = scope.get("raw_path")
        path = raw.decode("latin-1") if raw else (scope.get("path") or "")
        match = API_RE.match(path.split("?", 1)[0])
        if not match:
            return None
        try:
            owner, repo, operation = (
                urllib.parse.unquote(p, errors="strict")
                for p in match.groups())
        except UnicodeError:
            return None
        owner = owner.lower()
        if (operation not in READ_OPERATIONS
                and operation != VIEW_OPERATION) \
                or not OWNER_RE.match(owner) \
                or not REPO_RE.match(repo) or repo in (".", ".."):
            return None
        return owner, repo, operation

    async def handle(self, scope, send):
        """Serve the read and return True, or return False to fall through."""
        target = self.match(scope)
        if target is None:
            return False
        send = with_cors(scope, send)
        owner, repo, operation = target
        if operation == VIEW_OPERATION:
            from . import repo_views
            return await repo_views.handle(self, scope, send, owner, repo)
        pairs = gateway_query(
            operation, (scope.get("query_string") or b"").decode("latin-1"))
        if pairs is None:
            return False
        url = self.gateway_url(owner, repo, operation, pairs)
        policy = cache_control(operation, pairs)
        if policy == IMMUTABLE and operation != "raw":
            entry = self.immutable.get(url)
            if entry is not None:
                self.stats["memory"] += 1
                await self._send_memory(scope, send, entry)
                return True
        client = self.client()
        upstream = None
        try:
            upstream = await client.send(client.build_request("GET", url),
                                         stream=True)
            if upstream.status_code != 200:
                raise LookupError(upstream.status_code)
            length = int(upstream.headers.get("content-length") or 0)
            buffered = operation != "raw" or 0 < length <= RAW_BUFFER_MAX
            body = await upstream.aread() if buffered else None
        except asyncio.CancelledError:
            if upstream is not None:
                await upstream.aclose()
            raise
        except Exception:
            # Unknown/private repository (404), gateway error or timeout:
            # the Worker gives the canonical answer.
            if upstream is not None:
                await upstream.aclose()
            self.stats["fallthrough"] += 1
            return False
        if operation == "sizes" and body is not None:
            body = trim_sizes(body, scope.get("query_string") or b"")
        if body is not None and policy == IMMUTABLE and operation != "raw":
            self.immutable.put(url, upstream.headers.get(
                "content-type", "application/json"), body)
        try:
            await self._send(scope, send, owner, repo, operation, pairs,
                             upstream, body, length)
        finally:
            await upstream.aclose()
        return True

    async def _send_memory(self, scope, send, entry):
        """A commit-pinned answer from host memory (ImmutableCache)."""
        content_type, body = entry[0], entry[1]
        etag = etag_for(body)
        headers = [(b"x-content-type-options", b"nosniff"),
                   (b"x-forkmesh-local-reads", b"1"),
                   (b"content-type", content_type.encode("latin-1")),
                   (b"cache-control", IMMUTABLE.encode()),
                   (b"etag", etag.encode())]
        if self._node:
            headers.append((b"x-forkmesh-served-by", self._node.encode()))
        if _if_none_match(scope.get("headers") or [], etag):
            self.stats["not_modified"] += 1
            await send({"type": "http.response.start", "status": 304,
                        "headers": [h for h in headers
                                    if h[0] != b"content-type"]})
            await send({"type": "http.response.body", "body": b""})
            return
        headers.append((b"content-length", str(len(body)).encode()))
        await send({"type": "http.response.start", "status": 200,
                    "headers": headers})
        method = scope.get("method", "GET").upper()
        await send({"type": "http.response.body",
                    "body": b"" if method == "HEAD" else body})
        self.stats["served"] += 1

    async def _send(self, scope, send, owner, repo, operation, pairs,
                    upstream, body, length):
        headers = [(b"x-content-type-options", b"nosniff"),
                   (b"x-forkmesh-local-reads", b"1")]
        for name in PASS_HEADERS:
            value = upstream.headers.get(name)
            if value and not (name == "content-length" and body is not None):
                headers.append((name.encode(), value.encode("latin-1")))
        node = await self.node_name()
        if node:
            headers.append((b"x-forkmesh-served-by", node.encode()))
        if operation == "tree" and not dict(pairs).get("ref"):
            branch = await self._default_branch_quick(owner, repo, body)
            if branch:
                headers.append((b"x-forkmesh-ref",
                                branch.encode("latin-1", "replace")))
        method = scope.get("method", "GET").upper()
        if body is None:
            headers.append((b"cache-control", b"no-store"))
            if length:
                headers.append((b"content-length", str(length).encode()))
            await send({"type": "http.response.start", "status": 200,
                        "headers": headers})
            if method != "HEAD":
                async for chunk in upstream.aiter_raw():
                    await send({"type": "http.response.body", "body": chunk,
                                "more_body": True})
            await send({"type": "http.response.body", "body": b""})
            self.stats["served"] += 1
            return
        etag = etag_for(body)
        headers += [(b"cache-control", cache_control(operation, pairs).encode()),
                    (b"etag", etag.encode())]
        if _if_none_match(scope.get("headers") or [], etag):
            self.stats["not_modified"] += 1
            await send({"type": "http.response.start", "status": 304,
                        "headers": [h for h in headers
                                    if h[0] != b"content-type"]})
            await send({"type": "http.response.body", "body": b""})
            return
        headers.append((b"content-length", str(len(body)).encode()))
        await send({"type": "http.response.start", "status": 200,
                    "headers": headers})
        await send({"type": "http.response.body",
                    "body": b"" if method == "HEAD" else body})
        self.stats["served"] += 1

    async def _default_branch_quick(self, owner, repo, body):
        """default_branch() bounded to 150 ms so a cold branch listing never
        delays the tree it labels (the lookup finishes in the background)."""
        try:
            commit = str(json.loads(body).get("commit") or "")
        except Exception:
            return ""
        if not commit:
            return ""
        task = asyncio.ensure_future(self.default_branch(owner, repo, commit))
        try:
            return await asyncio.wait_for(asyncio.shield(task), 0.15)
        except asyncio.TimeoutError:
            return ""
