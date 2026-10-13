"""GET /api/repositories must not run the catalog-wide D1 housekeeping sweeps
inline on ANY request path.

Those sweeps already run on their own staggered cron (minute%15==9 /
minute%15==4), and the response is correct without them: blocked entries are
dropped by _is_blocked_catalog_identity, while stale signed catalog rows remain
visible with offline availability. Running housekeeping on every request held
the single Worker event loop long enough that the runtime canceled the request
as hung ("Cannot enter into task ..."), and on the 10s-cached anonymous miss it
re-ran every 10s per colo — a real contributor to the 2026-07-11 free-plan 1102
overload. Neither authenticated nor anonymous requests run it now; cron owns
housekeeping.
"""

import ast
import asyncio
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG_ROUTES = ENTRY.parent / "catalog_routes.py"


def _load_catalog_handler(extra_globals):
    tree = ast.parse(
        CATALOG_ROUTES.read_text(encoding="utf-8"),
        filename=str(CATALOG_ROUTES),
    )
    node = next(
        n for n in tree.body
        if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
        and n.name == "catalog_handler"
    )
    module = ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(CATALOG_ROUTES), "exec"), namespace)
    return namespace["catalog_handler"]


NOW = 1_700_000_000_000


def _run(url, *, cached=False):
    calls = {"purge_blocked": 0, "purge_stale": 0}
    repo = {
        "owner": "alice", "name": "repo", "visibility": "public",
        "rootCommit": "root", "updatedAt": str(NOW),
    }

    async def d1_all(env, sql, *args):
        if "FROM repositories" in sql:
            return [{"key_bi": "bi", "owner_bi": "obi", "data": repo}]
        return []  # host_presence -> nobody live

    async def purge_blocked_catalog(env):
        calls["purge_blocked"] += 1

    async def purge_stale_registered_nodes(env):
        calls["purge_stale"] += 1

    async def noop(*a, **k):
        return None

    async def identity(env, data):
        return data

    async def active_nodes(env):
        return None  # None => the active-node filter is skipped

    async def verify_token(env, viewer, ts, sig):
        return viewer  # any signed viewer authenticates in this harness

    async def edge_cache_match(_key):
        return "CACHED" if cached else None

    def json_response(payload, status=200, cache_seconds=None, cache_control=None,
                      extra_headers=None):
        return payload

    async def catalog_etag(env):
        return ""  # no stored rev in this harness -> conditional path is inert

    handler = _load_catalog_handler({
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "HOST_PRESENCE_STALE_MS": 10 * 60 * 1000,
        "HTTPS_MIRROR_STATUS_FRESH_MS": 10 * 60 * 1000,
        "MAX_CATALOG_REPOS": 200,
        "CATALOG_TTL": 10,
        "CATALOG_CACHE_KEY": "catalog",
        "method_name": (lambda req: req.method),
        "ensure_schema": noop,
        "purge_blocked_catalog": purge_blocked_catalog,
        "purge_stale_registered_nodes": purge_stale_registered_nodes,
        "active_registered_node_bis": active_nodes,
        "verify_catalog_view_token": verify_token,
        "_catalog_etag": catalog_etag,
        "blind_index": (lambda env, v: identity(env, "bi")),
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": noop,
        "_catalog_logical_owners": noop,
        "decrypt_row": identity,
        "_is_blocked_catalog_identity": (lambda env, o, n: False),
        "_admin_query": (lambda admin: ""),
        "_ssh_gateway_settings": (lambda env: {
            "configured": False, "host": "", "port": 0,
            "repositories": {},
        }),
        "safe_segment": (lambda v: v),
        "clean_string": (lambda v, n: v),
        "served_mirror_groups": (lambda repos: set()),
        "repo_clone_online": (lambda rec, served: False),
        "d1_all": d1_all,
        "json_response": json_response,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
    })

    request = SimpleNamespace(method="GET", url=url, headers={})
    result = asyncio.run(handler(None, request))
    return result, calls


def test_authenticated_catalog_skips_inline_purges():
    result, calls = _run(
        "https://forkmesh.internal/api/repositories"
        "?viewer=alice&ts=%d&sig=deadbeef" % NOW)
    assert result["ok"] is True
    # Authenticated request must not do inline write-side housekeeping.
    assert calls == {"purge_blocked": 0, "purge_stale": 0}


def test_anonymous_cache_miss_does_not_purge_inline():
    _result, calls = _run("https://forkmesh.internal/api/repositories")
    # Housekeeping is cron-only now — no inline sweep even on an anon miss.
    assert calls == {"purge_blocked": 0, "purge_stale": 0}


def test_anonymous_cache_hit_skips_purges():
    result, calls = _run(
        "https://forkmesh.internal/api/repositories", cached=True)
    assert result == "CACHED"
    assert calls == {"purge_blocked": 0, "purge_stale": 0}
