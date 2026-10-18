#!/usr/bin/env python3
"""Catalog availability comes only from fresh signed HTTPS mirror leases."""

import ast
import asyncio
import copy
import importlib.util
import re
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, urlparse


WORKER = Path(__file__).resolve().parents[1]
ENTRY = WORKER / "src" / "entry.py"
CATALOG_ROUTES = WORKER / "src" / "catalog_routes.py"
MIRRORS = WORKER / "src" / "mirrors.py"
NOW = 1_700_000_000_000
FRESH_MS = 10 * 60 * 1000


def _load_catalog_handler(extra_globals):
    tree = ast.parse(
        CATALOG_ROUTES.read_text(encoding="utf-8"),
        filename=str(CATALOG_ROUTES),
    )
    node = next(
        item for item in tree.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "catalog_handler"
    )
    namespace = dict(extra_globals)
    exec(
        compile(
            ast.fix_missing_locations(ast.Module(body=[node], type_ignores=[])),
            str(CATALOG_ROUTES),
            "exec",
        ),
        namespace,
    )
    return namespace["catalog_handler"]


def _mirror_helpers():
    spec = importlib.util.spec_from_file_location(
        "catalog_liveness_mirrors", MIRRORS)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.served_mirror_groups, module.repo_clone_online


def _endpoint(
        *, checked_at=NOW, verified_at=NOW, healthy=1, active=1,
        integrity="ok", abuse_blocked=0):
    return {
        "node_bi": "owner:mirror2",
        "checked_at": checked_at,
        "forkmesh_verified_at": verified_at,
        "healthy": healthy,
        "forkmesh_active": active,
        "integrity": integrity,
        "abuse_blocked": abuse_blocked,
    }


def _run(endpoint, records=None, copies=None, cores=()):
    records = records or [
        {
            "key_bi": "repo:source/forkmesh",
            "owner_bi": "owner:source",
            "data": {
                "owner": "source",
                "name": "forkmesh",
                "visibility": "public",
                "rootCommit": "root",
                "updatedAt": str(NOW - 1),
            },
        },
        {
            "key_bi": "repo:mirror2/forkmesh",
            "owner_bi": "owner:mirror2",
            "data": {
                "owner": "mirror2",
                "name": "forkmesh",
                "visibility": "public",
                "rootCommit": "root",
                "updatedAt": str(NOW),
            },
        },
    ]
    endpoints = [endpoint]
    endpoint_queries = []

    async def d1_all(_env, sql, *args):
        if "FROM repositories" in sql:
            return copy.deepcopy(records)
        if "FROM hosted_repos" in sql:
            if copies is None:
                raise RuntimeError("no such table: hosted_repos")
            return [dict(row) for row in copies if not row.get("empty")]
        if "FROM mirror_https_endpoints" in sql:
            endpoint_queries.append((sql, args))
            checked_cutoff, verified_cutoff = args
            return [
                {"node_bi": row["node_bi"]}
                for row in endpoints
                if (
                    row["checked_at"] >= checked_cutoff
                    and row["forkmesh_verified_at"] >= verified_cutoff
                    and row["healthy"] == 1
                    and row["forkmesh_active"] == 1
                    and row["integrity"] == "ok"
                    and row["abuse_blocked"] == 0
                )
            ]
        return []

    async def noop(*_args, **_kwargs):
        return None

    async def identity(_env, value):
        return value

    async def active_nodes(_env):
        # A retained catalog row is still visible when no node has fresh
        # liveness evidence. The old read-time filter would drop every row.
        return set()

    async def blind_index(_env, value):
        return "owner:" + str(value)

    async def no_cache(_key):
        return None

    served_mirror_groups, repo_clone_online = _mirror_helpers()
    extra = {}
    if cores:
        extra["hosted_core_nodes"] = lambda _env: list(cores)
    handler = _load_catalog_handler({
        **extra,
        "Date": type("D", (), {"now": staticmethod(lambda: NOW)}),
        "HTTPS_MIRROR_STATUS_FRESH_MS": FRESH_MS,
        "MAX_NODE_NAME": 80,
        "MAX_CATALOG_REPOS": 200,
        "CATALOG_TTL": 30,
        "CATALOG_CACHE_KEY": "catalog",
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
        "method_name": lambda request: request.method,
        "ensure_schema": noop,
        "active_registered_node_bis": active_nodes,
        "verify_catalog_view_token": noop,
        "blind_index": blind_index,
        "edge_cache_match": no_cache,
        "edge_cache_put": noop,
        "_catalog_logical_owners": noop,
        "decrypt_row": identity,
        "_is_blocked_catalog_identity": lambda _env, _owner, _repo: False,
        "_admin_query": lambda _value: "",
        "_ssh_gateway_settings": lambda _env: {
            "configured": False,
            "host": "",
            "port": 0,
            "repositories": {},
        },
        "safe_segment": lambda value: str(value or ""),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "served_mirror_groups": served_mirror_groups,
        "repo_clone_online": repo_clone_online,
        "d1_all": d1_all,
        "json_response": lambda payload, **_kwargs: payload,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
    })
    result = asyncio.run(handler(
        object(),
        SimpleNamespace(
            method="GET",
            url="https://forkmesh.internal/api/repositories?fresh=1",
            headers={},
        ),
    ))
    return {
        row["owner"]: row
        for row in result["repositories"]
    }, endpoint_queries


def test_fresh_signed_endpoint_marks_host_and_mirror_group_cloneable():
    repos, queries = _run(_endpoint())

    assert repos["mirror2"]["liveHost"] is True
    assert repos["mirror2"]["cloneOnline"] is True
    assert repos["source"]["liveHost"] is False
    assert repos["source"]["cloneOnline"] is True

    sql, args = queries[0]
    assert "forkmesh_verified_at>=?" in sql
    assert "healthy=1" in sql
    assert "forkmesh_active=1" in sql
    assert "integrity='ok'" in sql
    assert "abuse_blocked=0" in sql
    assert "host_presence" not in sql
    assert args == (NOW - FRESH_MS, NOW - FRESH_MS)


def test_stale_signed_endpoint_does_not_mark_catalog_online():
    repos, _queries = _run(_endpoint(
        checked_at=NOW - FRESH_MS - 1,
        verified_at=NOW - FRESH_MS - 1,
    ))

    assert repos["mirror2"]["liveHost"] is False
    assert repos["mirror2"]["cloneOnline"] is False
    assert repos["source"]["cloneOnline"] is False


def test_unverified_or_legacy_integrity_endpoint_does_not_mark_online():
    for endpoint in (
        _endpoint(verified_at=0),
        _endpoint(active=0),
        _endpoint(healthy=0),
        _endpoint(integrity="unknown"),
        _endpoint(integrity="verified"),
        _endpoint(abuse_blocked=1),
    ):
        repos, _queries = _run(endpoint)
        assert repos["mirror2"]["liveHost"] is False
        assert repos["mirror2"]["cloneOnline"] is False


def test_endpoint_queries_use_the_health_writer_canonical_integrity_label():
    source = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

    assert "integrity='verified'" not in source
    assert "integrity IN ('ok','verified')" not in source
    assert '"ok" if general_healthy else "degraded"' in source


def _hosted_record(owner="alice", name="project", source="hosted-web"):
    return {
        "key_bi": "repo:%s/%s" % (owner, name),
        "owner_bi": "owner:" + owner,
        "data": {
            "owner": owner,
            "name": name,
            "visibility": "public",
            "rootCommit": "hosted-root",
            "source": source,
            "updatedAt": str(NOW),
        },
    }


def test_hosted_repository_is_online_while_a_live_core_serves_its_copy():
    records = [_hosted_record()]
    copies = [{"key_bi": "repo:alice/project", "node": "mirror2"}]
    repos, _queries = _run(_endpoint(), records, copies, cores=("mirror2",))

    assert repos["alice"]["liveHost"] is True
    assert repos["alice"]["cloneOnline"] is True


def test_hosted_repository_stays_offline_without_a_live_serving_copy():
    records = [_hosted_record()]
    for copies, endpoint in (
        ([], _endpoint()),
        ([{"key_bi": "repo:alice/project", "node": "mirror2", "empty": 1}],
         _endpoint()),
        ([{"key_bi": "repo:alice/project", "node": "mirror2"}],
         _endpoint(healthy=0)),
        ([{"key_bi": "repo:other/project", "node": "mirror2"}],
         _endpoint()),
    ):
        repos, _queries = _run(endpoint, records, copies, cores=("mirror2",))
        assert repos["alice"]["liveHost"] is False


def test_only_hosted_records_take_liveness_from_core_copies():
    records = [_hosted_record(source="remote-clone")]
    copies = [{"key_bi": "repo:alice/project", "node": "mirror2"}]
    repos, _queries = _run(_endpoint(), records, copies, cores=("mirror2",))
    assert repos["alice"]["liveHost"] is False

    repos, _queries = _run(_endpoint(), [_hosted_record()], copies)
    assert repos["alice"]["liveHost"] is False

    repos, _queries = _run(_endpoint(), [_hosted_record()], None,
                           cores=("mirror2",))
    assert repos["alice"]["liveHost"] is False
