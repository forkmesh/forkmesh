from __future__ import annotations

import asyncio
import re
import urllib.parse

LONG_RE = re.compile(r"^/([^/?#]+)/([^/?#]+)/(tree|blob)/([^?#]+)$")
SHORT_RE = re.compile(r"^/([^/?#]+)/(tree|blob)/([^?#]+)$")
LOOKUP_SECONDS = 0.15
LISTING_SECONDS = 30.0
MAX_LISTINGS = 256
SAFE = "!'()*"

_listings = {}
_pending = {}


def parts(rest):
    return [urllib.parse.unquote(part) for part in str(rest or "").split("/")
            if part]


def split(kind, segments, refs):
    names = {str(ref).strip() for ref in refs or () if str(ref or "").strip()}
    segments = [segment for segment in segments if segment]
    longest = len(segments) - (1 if kind == "blob" else 0)
    for size in range(longest, 0, -1):
        ref = "/".join(segments[:size])
        if ref in names:
            return ref, "/".join(segments[size:])
    return None


def location(base, kind, ref, path, default):
    quoted = "/".join(urllib.parse.quote(part, safe=SAFE)
                      for part in path.split("/") if part)
    target = base + ("/%s/%s" % (kind, quoted) if quoted else "")
    if ref != default:
        target += "?ref=" + urllib.parse.quote(ref, safe=SAFE)
    return target


def _raw_path(scope):
    raw = scope.get("raw_path")
    return raw.decode("latin-1") if raw else (scope.get("path") or "")


def _plain_get(scope):
    return scope.get("method", "GET").upper() == "GET" and \
        not (scope.get("query_string") or b"")


def long_target(scope, reserved=frozenset()):
    if not _plain_get(scope):
        return None
    match = LONG_RE.match(_raw_path(scope))
    if not match:
        return None
    owner, repo, kind, rest = match.groups()
    if urllib.parse.unquote(owner).lower() in reserved or repo in (".", "..") \
            or repo.endswith(".git"):
        return None
    return (urllib.parse.unquote(owner).lower(), urllib.parse.unquote(repo),
            "/%s/%s" % (owner, repo), kind, rest)


def short_code(scope):
    if not _plain_get(scope):
        return None
    match = SHORT_RE.match(_raw_path(scope))
    if not match:
        return None
    name, kind, rest = match.groups()
    return "/" + name, kind, rest


async def _read_listing(fastpath, owner, repo):
    try:
        response = await fastpath.client().get(
            "/v1/public/repositories/%s/%s/branches" % (
                urllib.parse.quote(owner, safe=""),
                urllib.parse.quote(repo, safe="")))
        if response.status_code != 200:
            return None
        data = response.json()
    except Exception:
        return None
    default = str(data.get("defaultBranch") or "").strip()
    names = {str(branch.get("name") or "").strip()
             for branch in data.get("branches") or []
             if isinstance(branch, dict)}
    names.discard("")
    if default:
        names.add(default)
    return default, frozenset(names)


def _listing_task(fastpath, owner, repo):
    key = (owner, repo)
    loop = asyncio.get_running_loop()
    entry = _listings.get(key)
    if entry is not None and loop.time() - entry[0] < LISTING_SECONDS:
        done = loop.create_future()
        done.set_result(entry[1])
        return done
    pending = _pending.get(key)
    if pending is None:
        async def run():
            found = await _read_listing(fastpath, owner, repo)
            if found is not None:
                if len(_listings) > MAX_LISTINGS:
                    _listings.clear()
                _listings[key] = (asyncio.get_running_loop().time(), found)
            return found
        pending = asyncio.ensure_future(run())
        _pending[key] = pending
        pending.add_done_callback(lambda _f: _pending.pop(key, None))
    return pending


async def redirect(fastpath, owner, repo, base, kind, rest):
    if fastpath is None:
        return None
    segments = parts(rest)
    if not segments or (kind == "blob" and len(segments) < 2):
        return None
    try:
        found = await asyncio.wait_for(
            asyncio.shield(_listing_task(fastpath, owner, repo)),
            LOOKUP_SECONDS)
    except Exception:
        return None
    if not found:
        return None
    default, names = found
    hit = split(kind, segments, names)
    if hit is None:
        return None
    return location(base, kind, hit[0], hit[1], default)


def response(target):
    from . import js as _js
    return _js.Response(b"", 308, "", _js.Headers({
        "location": target, "cache-control": "no-store"}))
