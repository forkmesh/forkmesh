"""Worker wiring for signed, masked direct-HTTPS repository traffic."""

import ast
import asyncio
import hashlib
import hmac
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote


ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
CATALOG_ROUTES = ROOT / "app" / "src" / "catalog_routes.py"
STATUS_MONITORING = ROOT / "app" / "src" / "status_monitoring.py"
EDGE = ROOT / "app" / "src" / "edge_routing.py"
SCHEMA = ROOT / "app" / "src" / "schema.py"
URLS = ROOT / "app" / "src" / "urls.py"
QT_REPO_HOST = ROOT / "desktop" / "src" / "RepoHost.cpp"
MIGRATION = (
    ROOT / "app" / "migrations"
    / "0047_https_mirror_routing.sql"
)
PRIVATE_MIGRATION = (
    ROOT / "app" / "migrations"
    / "0050_private_mirror_routes.sql"
)
PRIVATE_ACCESS_MIGRATION = (
    ROOT / "app" / "migrations"
    / "0053_private_replica_access.sql"
)
WRANGLER = ROOT / "app" / "wrangler.toml"

SRC = ROOT / "app" / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import edge_routing as routing  # noqa: E402
import mirrors as mirror_policy  # noqa: E402
import releases as release_policy  # noqa: E402
import schema as relay_schema  # noqa: E402


def _function_source(name):
    for path in (ENTRY, CATALOG_ROUTES, STATUS_MONITORING):
        source = path.read_text(encoding="utf-8")
        tree = ast.parse(source)
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                if node.name == name:
                    return ast.get_source_segment(source, node)
    raise AssertionError(f"{name} not found")


def _method_source(class_name, method_name):
    source = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    tree = ast.parse(source)
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for child in node.body:
                if (
                    isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef))
                    and child.name == method_name
                ):
                    return ast.get_source_segment(source, child)
    raise AssertionError(f"{class_name}.{method_name} not found")


def _public_context_cache_namespace(*, ttl=5_000, maximum=2):
    namespace = {
        "HTTPS_MIRROR_PUBLIC_CONTEXT_TTL_MS": ttl,
        "HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO_MAX": maximum,
        "_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO": {},
    }
    for name in (
        "_https_mirror_public_context_copy",
        "_https_mirror_public_context_cache_get",
        "_https_mirror_public_context_cache_put",
        "_https_mirror_public_context_cache_clear",
    ):
        exec(_function_source(name), namespace)
    return namespace


def test_public_routing_context_memo_is_bounded_copied_and_privacy_ttl_scoped():
    namespace = _public_context_cache_namespace()
    put = namespace["_https_mirror_public_context_cache_put"]
    get = namespace["_https_mirror_public_context_cache_get"]
    clear = namespace["_https_mirror_public_context_cache_clear"]
    context = {
        "owner": "forkmesh",
        "repo": "forkmesh",
        "nodes": {"mirror9"},
        "groupNodes": {"mirror9", "mirror14"},
        "currentNodes": {"mirror9"},
        "pins": {"a" * 64},
        "currentPins": {"a" * 64},
        "repoBi": "repo-bi",
    }

    returned = put("forkmesh", "forkmesh", context, 10_000)
    returned["nodes"].add("untrusted")
    assert get("forkmesh", "forkmesh", 15_000)["nodes"] == {"mirror9"}
    assert get("forkmesh", "forkmesh", 15_001) is None

    put("owner-a", "repo", context, 20_000)
    put("owner-b", "repo", context, 21_000)
    put("owner-c", "repo", context, 22_000)
    assert len(namespace["_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO"]) == 2
    assert get("owner-a", "repo", 22_000) is None
    clear()
    assert namespace["_HTTPS_MIRROR_PUBLIC_CONTEXT_MEMO"] == {}


def test_public_routing_context_memo_wraps_expensive_catalog_work_and_purges():
    context = _function_source("_https_mirror_public_context")
    assert context.index("_https_mirror_public_context_cache_get") < context.index(
        "_decrypted_public_catalog"
    )
    assert "_https_mirror_public_context_cache_put" in context
    assert (
        "HTTPS_MIRROR_PUBLIC_CONTEXT_TTL_MS = PUBLIC_CATALOG_MEMO_TTL_MS"
        in ENTRY.read_text(encoding="utf-8")
    )
    purge = _function_source("purge_catalog_related_caches")
    assert purge.index("_https_mirror_public_context_cache_clear") < purge.index(
        "edge_cache_delete"
    )


def test_internal_repository_probes_reuse_context_without_traffic_accounting():
    routed = _function_source("_routed_repository_read")
    proxy = _function_source("_https_mirror_proxy")
    flagship = _function_source("_flagship_repository_probe")
    assert "context_cache=None" in routed
    assert "context_cache=context_cache" in routed
    assert "context_cache=None" in proxy
    assert "context_cache.get(context_key)" in proxy
    assert "context_cache[context_key]" in proxy
    assert "if status == 404:" in flagship
    assert "Do not immediately replay a 5xx" in flagship
    assert "status, text, served_by = await read(routing_contexts)" in flagship
    assert "{}, retry_failed_proofs=True" in flagship
    assert '"&ref=" + quote(snapshot_ref)' in flagship
    assert flagship.count("monitoring=True") == 2
    assert "retry_failed_proofs=retry_failed_proofs" in routed
    assert "retry_failed=retry_failed_proofs" in proxy
    assert "if not monitoring:" in proxy
    assert flagship.count("_cron_subrequest_charge()") == 1


def test_deep_route_admission_override_kv_read_is_counted_once():
    charges = []

    class KV:
        async def get(self, key):
            assert key == "admission-override:repo-bi"
            return "active"

    namespace = {
        "ADMISSION_OVERRIDE_PREFIX": "admission-override:",
        "_cron_subrequest_charge": lambda: charges.append(1),
    }
    exec(_function_source("_admission_override_active"), namespace)
    active = namespace["_admission_override_active"]
    env = SimpleNamespace(REPOSITORY_METADATA=KV())
    assert asyncio.run(active(env, "repo-bi"))
    assert charges == [1]


def test_monitoring_candidate_snapshot_is_copied_and_request_scoped():
    builds = []

    async def build(_env, context, region, sticky="", avoid_node="",
              operation=""):
        builds.append((context["repoBi"], region, sticky, avoid_node))
        return [{"node": "mirror9", "healthy": True}]

    namespace = {"_https_mirror_candidates": build}
    exec(_function_source("_https_mirror_request_candidates"), namespace)
    candidates = namespace["_https_mirror_request_candidates"]
    context = {
        "repoBi": "flagship-bi",
        "owner": "forkmesh",
        "repo": "forkmesh",
    }
    suite_cache = {}

    first = asyncio.run(candidates(
        object(), context, "us", monitoring=True,
        context_cache=suite_cache))
    first[0]["healthy"] = False
    for _ in range(10):
        result = asyncio.run(candidates(
            object(), context, "US", monitoring=True,
            context_cache=suite_cache))
        assert result == [{"node": "mirror9", "healthy": True}]
    assert builds == [("flagship-bi", "US", "", "")]

    # A new probe dictionary gets a fresh endpoint decision. Ordinary traffic
    # never consumes or populates the suite cache, even if one is supplied.
    asyncio.run(candidates(
        object(), context, "US", monitoring=True, context_cache={}))
    asyncio.run(candidates(
        object(), context, "US", monitoring=False,
        context_cache=suite_cache))
    asyncio.run(candidates(
        object(), context, "US", monitoring=False,
        context_cache=suite_cache))
    assert len(builds) == 4


def test_mirror_reconciliation_candidate_excludes_requesting_node():
    now = 1_800_000_000_000

    def endpoint(node):
        return {
            "node_name": node,
            "base_url": "https://%s.example.test" % node,
            "public_key": "public-key",
            "registration_sig": "registration",
            "issued_at": 1,
            "checked_at": now,
            "latency_ms": 10,
            "region": "US",
            "healthy": 1,
            "integrity": "ok",
            "abuse_blocked": 0,
            "forkmesh_refs_sha256": "a" * 64,
        }

    async def d1_all(_env, _sql, *_params):
        return [endpoint("mirror24"), endpoint("mirror25")]

    namespace = {
        "d1_all": d1_all,
        "_random_bytes": lambda size: bytes(size),
        "Date": type("D", (), {"now": staticmethod(lambda: now)}),
        "https_routing": routing,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
    }
    _load_avoid_helpers(namespace)
    exec(_function_source("_https_mirror_endpoint_projection"), namespace)
    exec(_function_source("_https_mirror_candidates"), namespace)
    result = asyncio.run(namespace["_https_mirror_candidates"](
        object(),
        {"nodes": {"mirror24", "mirror25"}, "roundRobinAll": True},
        "US",
        avoid_node="mirror25",
    ))
    assert [item["node"] for item in result] == ["mirror24"]


def _load_avoid_helpers(namespace):
    from urllib.parse import urlparse

    node_name = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
    namespace.update({
        "urlparse": urlparse,
        "valid_node_name": lambda value: bool(node_name.match(value or "")),
        "MAX_AVOIDED_MIRROR_NODES": 4,
    })
    exec(_function_source("_https_mirror_avoided_nodes"), namespace)
    exec(_function_source("_https_mirror_record_avoided"), namespace)


def test_mirror_avoid_list_matches_node_names_and_endpoint_host_labels():
    namespace = {}
    _load_avoid_helpers(namespace)
    avoided = namespace["_https_mirror_avoided_nodes"](
        " fmnext-n3 , N2,bad node,n2,x,y,z")
    assert avoided == {"fmnext-n3", "n2", "x", "y"}
    record_avoided = namespace["_https_mirror_record_avoided"]
    assert record_avoided({"node": "fmnext-n3"}, avoided)
    assert record_avoided(
        {"node": "fmnext-n2",
         "baseUrl": "https://n2.next.forkmesh.com"}, avoided)
    assert not record_avoided(
        {"node": "fmnext-n1",
         "baseUrl": "https://n1.next.forkmesh.com"}, avoided)


def test_public_context_history_keys_are_authority_priority_and_hard_capped():
    namespace = {
        "repo_mirror_pin_history_keys":
            mirror_policy.repo_mirror_pin_history_keys,
    }
    exec(_function_source("_https_mirror_history_keys"), namespace)
    history_keys = namespace["_https_mirror_history_keys"]
    members = [
        {
            "key_bi": "key-%03d" % index,
            "data": {
                "owner": "node-%03d" % index,
                "name": "forkmesh",
                "rootCommit": "root",
                "source": "local-node",
            },
        }
        for index in reversed(range(12))
    ]
    selected = history_keys(members, "key-011")
    assert selected == ["key-011", "key-000", "key-001", "key-002"]
    assert len(selected) == mirror_policy.REPO_STATE_HISTORY_KEY_LIMIT
    assert history_keys(members, "key-011", linked_canonical=True) == [
        "key-011"
    ]

    context = _function_source("_https_mirror_public_context")
    assert "_https_mirror_history_keys(" in context
    assert "linked_canonical=bool(linked_target)" in context
    assert "await _bounded_repo_state_history(env, group_keys)" in context


def test_repo_state_history_read_is_index_backed_row_bounded_and_within_quota():
    connection = sqlite3.connect(":memory:")
    connection.row_factory = sqlite3.Row
    try:
        for statement in relay_schema.SCHEMA_STATEMENTS:
            if "repo_state_history" in statement:
                connection.execute(statement)
        for key_index in range(6):
            key = "key-%d" % key_index
            connection.executemany(
                "INSERT INTO repo_state_history (key_bi,state_hash,ts) "
                "VALUES (?,?,?)",
                [
                    (key, "%s-state-%03d" % (key, row_index), row_index)
                    for row_index in range(150)
                ],
            )

        issued = []

        async def d1_all(_env, sql, *params):
            issued.append((sql, params))
            return [
                dict(row) for row in connection.execute(sql, params).fetchall()
            ]

        namespace = {
            "d1_all": d1_all,
            "REPO_STATE_HISTORY_KEY_LIMIT":
                mirror_policy.REPO_STATE_HISTORY_KEY_LIMIT,
            "STATE_PIN_HISTORY": mirror_policy.STATE_PIN_HISTORY,
        }
        exec(_function_source("_bounded_repo_state_history"), namespace)
        history = asyncio.run(namespace["_bounded_repo_state_history"](
            object(), ["key-0", "key-1", "key-2", "key-3", "key-4", "key-5"]
        ))

        sql, params = issued.pop()
        assert set(history) == {"key-0", "key-1", "key-2", "key-3"}
        assert sum(len(rows) for rows in history.values()) == 400
        assert all(len(rows) == 100 for rows in history.values())
        assert history["key-0"][0] == "key-0-state-149"
        assert history["key-0"][-1] == "key-0-state-050"
        assert sql.count("FROM repo_state_history") == 4
        assert sql.count("ORDER BY ts DESC LIMIT ?") == 4
        assert params == (
            "key-0", 100, "key-1", 100,
            "key-2", 100, "key-3", 100,
        )

        plan = [
            row[3] for row in connection.execute(
                "EXPLAIN QUERY PLAN " + sql, params)
        ]
        indexed_search = (
            "SEARCH repo_state_history USING COVERING INDEX "
            "idx_repo_state_history_key_ts (key_bi=?)"
        )
        assert plan.count(indexed_search) == 4
        assert not any(
            detail == "SCAN repo_state_history" for detail in plan
        )

        scheduled_calls_per_day = 24 * 60 + 2 * (24 * 60 // 5)
        worst_history_rows_per_day = (
            mirror_policy.REPO_STATE_HISTORY_KEY_LIMIT
            * mirror_policy.STATE_PIN_HISTORY
            * scheduled_calls_per_day
        )
        assert scheduled_calls_per_day == 2_016
        assert worst_history_rows_per_day == 806_400
        assert worst_history_rows_per_day < 5_000_000
    finally:
        connection.close()


def test_monitoring_failures_never_mutate_shared_endpoint_eligibility():
    writes = []

    async def d1_run(_env, sql, *args):
        writes.append((sql, args))

    namespace = {
        "d1_run": d1_run,
        "Date": type("D", (), {"now": staticmethod(lambda: 1_700_000_000_000)}),
    }
    exec(_function_source("_https_mirror_record_request_rejection"), namespace)
    record = namespace["_https_mirror_record_request_rejection"]
    endpoint = {"node": "mirror9"}

    for status in (0, 401, 403, 502, 503):
        assert not asyncio.run(record(
            object(), endpoint, status, monitoring=True))
    assert writes == []

    for status in (0, 502, 503):
        assert not asyncio.run(record(object(), endpoint, status))
    assert writes == []
    assert asyncio.run(record(object(), endpoint, 401))
    assert asyncio.run(record(object(), endpoint, 403))
    assert len(writes) == 2
    assert all("forkmesh_active=0" in sql for sql, _args in writes)


def test_public_repository_metadata_cache_is_attestation_keyed_and_bounded():
    namespace = {
        "hashlib": hashlib,
        "json": json,
        "re": re,
        "MAX_BLOB_BATCH": 60,
        "REPOSITORY_METADATA_CACHE_PREFIX": (
            "https://forkmesh.internal/repository-metadata/v3/"
        ),
        "REPOSITORY_METADATA_FALLBACK_PREFIX": (
            "https://forkmesh.internal/repository-metadata-fallback/v1/"
        ),
    }
    exec(_function_source("repository_metadata_cache_key"), namespace)
    key = namespace["repository_metadata_cache_key"]
    first = {
        "repoBi": "public-repo-blind-index",
        "currentPins": {"b" * 64, "a" * 64},
    }
    same = {
        "repoBi": "public-repo-blind-index",
        "currentPins": {"a" * 64, "b" * 64},
    }
    pull_ref = "c" * 40

    tree_key = key(first, "tree", {"path": ".forkmesh/pulls", "ref": pull_ref})
    assert tree_key.startswith(namespace["REPOSITORY_METADATA_CACHE_PREFIX"])
    assert tree_key == key(
        same, "tree", {"path": ".forkmesh/pulls", "ref": pull_ref}
    )
    assert tree_key != key(
        {**first, "currentPins": {"d" * 64}},
        "tree",
        {"path": ".forkmesh/pulls", "ref": pull_ref},
    )
    assert tree_key != key(
        first, "tree", {"path": ".forkmesh/pulls", "ref": "d" * 40}
    )
    fallback_key = key(
        first, "tree", {"path": ".forkmesh/pulls", "ref": pull_ref},
        fallback=True,
    )
    assert fallback_key.startswith(
        namespace["REPOSITORY_METADATA_FALLBACK_PREFIX"])
    assert fallback_key == key(
        {**first, "currentPins": {"d" * 64}},
        "tree", {"path": ".forkmesh/pulls", "ref": pull_ref},
        fallback=True,
    )
    assert fallback_key != key(
        first, "tree", {"path": ".forkmesh/pulls", "ref": "d" * 40},
        fallback=True,
    )
    assert key(first, "branches", {})
    assert key(first, "tree", {"path": "", "ref": "main"})
    assert key(first, "tree", {"path": "", "ref": "Feature/X"}) != key(
        first, "tree", {"path": "", "ref": "feature/x"}
    )
    assert key(first, "sizes", {"ref": "main"})
    assert key(first, "stats", {"ref": "main"})
    assert key(first, "history", {"ref": "main"})
    assert key(first, "blob", {"path": "README.md", "ref": "main"})
    assert key(
        first,
        "blob",
        {
            "path": ".forkmesh/issues/open/44/issue-44.json",
            "ref": "main",
        },
    )
    assert key(
        first,
        "tree",
        {"path": ".forkmesh/discussions/9", "ref": "main"},
    )
    assert key(
        first,
        "blob",
        {"path": ".forkmesh/pulls/44/changes.patch", "ref": pull_ref},
    )
    assert key(
        first,
        "blobs",
        {
            "path": [".forkmesh/pulls/44/pull.md", ".forkmesh/pulls/43/pull.md"],
            "ref": pull_ref,
        },
    )
    assert key(
        first,
        "blobs",
        {
            "path": [
                ".forkmesh/issues/open/2/issue-2.json",
                ".forkmesh/issues/open/1/issue-1.json",
            ],
            "ref": "main",
        },
    )
    assert key(
        first,
        "blobs",
        {
            "path": [
                ".forkmesh/discussions/3/discussion.md",
                ".forkmesh/discussions/2/discussion.md",
            ],
            "ref": "main",
        },
    )

    # No arbitrary source contents, raw paths, malformed refs, duplicate
    # amplification, or unattested repository state may enter this cache.
    assert not key(
        first,
        "blobs",
        {"path": ["src/main.py"], "ref": pull_ref},
    )
    assert not key(
        first,
        "blobs",
        {
            "path": [".forkmesh/pulls/44/pull.md", ".forkmesh/pulls/44/pull.md"],
            "ref": pull_ref,
        },
    )
    assert not key(first, "blob", {"path": "src/main.py", "ref": "main"})
    assert not key(first, "tree", {"path": "private", "ref": pull_ref})
    assert not key(first, "tree", {"path": ".forkmesh/pulls", "ref": "bad..ref"})
    assert not key(
        {"repoBi": "public-repo-blind-index", "currentPins": set()},
        "branches",
        {},
    )

    proxy = _function_source("_https_mirror_proxy")
    assert (
        "request_bypasses_repository_metadata_cache(request)" in proxy
    )
    assert (
        proxy.index("repository_metadata_cache_get(env, metadata_cache_key)")
        < proxy.index("_https_mirror_request_candidates(")
    )
    assert "not monitoring" in proxy
    assert "metadata_fallback_key" in proxy
    assert (
        "repository_metadata_cache_put(\n"
        "                    env, cache_key, upstream, status)"
    ) in proxy
    assert 'in context.get("currentPins", set())' in proxy
    assert (
        'response_headers["X-ForkMesh-Served-By"] = endpoint["node"]'
        in proxy
    )
    assert (
        proxy.index("repository_metadata_cache_get(env, metadata_cache_key)")
        < proxy.index('response_headers["X-ForkMesh-Served-By"]')
    )
    get_source = _function_source("repository_metadata_cache_get")
    put_source = _function_source("repository_metadata_cache_put")
    assert '"no-store, max-age=0, must-revalidate"' in get_source
    assert "namespace.get(cache_key)" in get_source
    assert "namespace.put(cache_key, raw)" in put_source
    assert '"public, max-age=%d, immutable"' in put_source
    assert "int(status or 0) != 200" in put_source
    assert "content_length <= 0" in put_source
    assert "content_length > REPOSITORY_METADATA_CACHE_MAX_BYTES" in put_source
    assert "X-ForkMesh-Served-By" not in get_source
    assert "existing = await js_caches.default.match(cache_key)" in put_source

    candidates = _function_source("_https_mirror_candidates")
    projection = _function_source("_https_mirror_endpoint_projection")
    assert "forkmesh_refs_sha256" in candidates
    assert '"refsSha256": row.get("forkmesh_refs_sha256")' in projection
    assert "equivalent_current_endpoint_nodes" in candidates

    class MustNotClone:
        def clone(self):
            raise AssertionError("a non-200 upstream must never be cached")

    put_namespace = {
        "asyncio": asyncio,
        "json": json,
        "REPOSITORY_METADATA_CACHE_MAX_BYTES": 8 * 1024 * 1024,
        "REPOSITORY_METADATA_CACHE_TTL": 365 * 24 * 60 * 60,
    }
    exec(put_source, put_namespace)
    assert asyncio.run(
        put_namespace["repository_metadata_cache_put"](
            SimpleNamespace(),
            "https://cache.invalid/key",
            MustNotClone(),
            503,
        )
    ) is None


def test_named_ref_metadata_cache_entries_expire_when_pins_stop_moving():
    # 2026-09-14: the flagship's attested pin set had not advanced since the
    # previous evening, so every read of `tree?path=.forkmesh/pulls&ref=
    # forkmesh/pulls` kept hitting the entry stored then. The site listed
    # yesterday's ledger (every pull merged) while all three mirrors served
    # today's open pulls. A named ref must expire; a commit id must not.
    namespace = {
        "hashlib": hashlib,
        "json": json,
        "re": re,
        "MAX_BLOB_BATCH": 60,
        "REPOSITORY_METADATA_CACHE_PREFIX": (
            "https://forkmesh.internal/repository-metadata/v3/"
        ),
        "REPOSITORY_METADATA_FALLBACK_PREFIX": (
            "https://forkmesh.internal/repository-metadata-fallback/v1/"
        ),
        "REPOSITORY_METADATA_NAMED_REF_WINDOW_MS": 60 * 1000,
    }
    exec(_function_source("repository_metadata_cache_key"), namespace)
    key = namespace["repository_metadata_cache_key"]
    context = {"repoBi": "repo-bi", "currentPins": {"a" * 64}}
    ledger = {"path": ".forkmesh/pulls", "ref": "forkmesh/pulls"}
    t0 = 1_789_400_000_000

    # Within one window the same named read shares an entry.
    assert key(context, "tree", ledger, now=t0) == key(
        context, "tree", ledger, now=t0 + 1_000)
    # A day later, with identical pins, it no longer does.
    assert key(context, "tree", ledger, now=t0) != key(
        context, "tree", ledger, now=t0 + 86_400_000)
    # The default branch and the branch list are named reads too.
    assert key(context, "tree", {"path": ""}, now=t0) != key(
        context, "tree", {"path": ""}, now=t0 + 120_000)
    assert key(context, "branches", {}, now=t0) != key(
        context, "branches", {}, now=t0 + 120_000)
    # A commit-addressed read is immutable and keeps one key forever.
    pinned = {"path": ".forkmesh/pulls", "ref": "c" * 40}
    assert key(context, "tree", pinned, now=t0) == key(
        context, "tree", pinned, now=t0 + 86_400_000)
    assert key(context, "tree", pinned, now=t0) == key(context, "tree", pinned)
    # The outage fallback still serves the last attested view.
    assert key(context, "tree", ledger, fallback=True, now=t0) == key(
        context, "tree", ledger, fallback=True, now=t0 + 86_400_000)


def test_every_spelling_of_a_directory_reads_and_caches_as_one_path():
    # Issue #628 reported `tree?path=.` answering 503 while the README blob
    # beside it answered 200 from the metadata cache. That spelling of the
    # repository root reaches the gateway literally, where it is not a path at
    # all, and repository_metadata_cache_key never admitted it either — so it
    # could neither be served from the attested snapshot during an outage nor
    # fill it while mirrors were healthy.
    namespace = {
        "parse_qs": parse_qs,
        "MAX_BLOB_BATCH": 60,
        "HTTPS_MIRROR_REF_OPERATIONS": frozenset({
            "tree", "blobs", "blob", "raw", "history", "search", "stats",
            "sizes",
        }),
    }
    exec(_function_source("_repository_read_path"), namespace)
    exec(_function_source("_https_mirror_request_query"), namespace)
    read_path = namespace["_repository_read_path"]
    request_query = namespace["_https_mirror_request_query"]

    for spelling in (".", "./", "/", "", "//"):
        assert read_path(spelling) == ""
    for spelling in ("./src/main", "/src/main", "src/main/", "src//main"):
        assert read_path(spelling) == "src/main"
    # Traversal is left spelled exactly as asked so it keeps failing closed at
    # the gateway instead of being resolved into a real path here.
    assert read_path("src/../../etc/passwd") == "src/../../etc/passwd"

    def query(raw, operation="tree"):
        return request_query(_urlparse("https://x.invalid/?" + raw), operation)

    assert query("path=.") == {"path": ""}
    assert query("path=") == {"path": ""}
    assert query("path=./src&ref=main") == {"path": "src", "ref": "main"}
    assert query("path=./README.md", "blob") == {"path": "README.md"}
    assert query("path=./a.md&path=b/", "blobs") == {"path": ["a.md", "b"]}
    # A search string is a query, not a path, and must never be canonicalized.
    assert query("q=./src", "search") == {"path": "./src"}
    assert query("ref=feature%2Fphone", "history") == {"ref": "feature/phone"}
    assert query("", "history") == {}
    assert query("ref=feature", "commit") == {}

    cache_namespace = {
        "hashlib": hashlib,
        "json": json,
        "re": re,
        "MAX_BLOB_BATCH": 60,
        "REPOSITORY_METADATA_CACHE_PREFIX": (
            "https://forkmesh.internal/repository-metadata/v3/"
        ),
    }
    exec(_function_source("repository_metadata_cache_key"), cache_namespace)
    key = cache_namespace["repository_metadata_cache_key"]
    context = {"repoBi": "repo-bi", "currentPins": {"a" * 64}}
    assert key(context, "tree", query("path=.")) == key(
        context, "tree", query("path="))
    assert key(context, "tree", query("path=."))
    assert key(context, "history", query("ref=main", "history")) != key(
        context, "history", query("ref=feature", "history"))


def test_live_repository_reads_bypass_cache_for_round_robin_accounting():
    namespace = {}
    exec(
        _function_source("request_bypasses_repository_metadata_cache"),
        namespace,
    )
    bypasses = namespace["request_bypasses_repository_metadata_cache"]

    def request(**headers):
        return SimpleNamespace(headers=headers)

    assert bypasses(request(**{"cache-control": "no-cache"}))
    assert bypasses(request(**{"cache-control": "public, no-store"}))
    assert bypasses(request(pragma="no-cache"))
    assert not bypasses(request(**{"cache-control": "max-age=3600"}))
    assert not bypasses(request())


def test_repository_metadata_cache_round_trips_through_global_kv():
    config = WRANGLER.read_text(encoding="utf-8")
    assert 'binding = "REPOSITORY_METADATA"' in config
    assert 'id = "REPLACE_NEXT_KV_REPOSITORY_METADATA_ID"' in config

    class FakeHeaders(dict):
        pass

    class FakeUpstream:
        headers = FakeHeaders({
            "content-type": "application/json; charset=utf-8",
        })

        def clone(self):
            return self

        async def text(self):
            return '{"ok":true,"entries":[]}'

    class FakeCache:
        def __init__(self):
            self.values = {}

        async def match(self, key):
            return self.values.get(key)

        async def put(self, key, value):
            self.values[key] = value

    class FakeKV:
        def __init__(self):
            self.values = {}

        async def get(self, key):
            return self.values.get(key)

        async def put(self, key, value):
            self.values[key] = value

    class FakeJsResponse:
        @staticmethod
        def new(body, options):
            return SimpleNamespace(
                body=body,
                headers=FakeHeaders(options["headers"]),
            )

    class FakeWorkerResponse:
        def __init__(self, body, status=200, headers=None):
            self.body = body
            self.status = status
            self.headers = headers or {}

    edge = FakeCache()
    kv = FakeKV()
    namespace = {
        "asyncio": asyncio,
        "json": json,
        "js_caches": SimpleNamespace(default=edge),
        "JsResponse": FakeJsResponse,
        "Response": FakeWorkerResponse,
        "to_js": lambda value: value,
        "REPOSITORY_METADATA_CACHE_MAX_BYTES": 8 * 1024 * 1024,
        "REPOSITORY_METADATA_CACHE_TTL": 365 * 24 * 60 * 60,
    }
    exec(_function_source("repository_metadata_cache_put"), namespace)
    exec(_function_source("repository_metadata_cache_get"), namespace)
    cache_key = "https://forkmesh.internal/repository-metadata/v3/" + "a" * 64
    env = SimpleNamespace(REPOSITORY_METADATA=kv)

    asyncio.run(namespace["repository_metadata_cache_put"](
        env, cache_key, FakeUpstream(), 200))
    assert kv.values[cache_key] == '{"ok":true,"entries":[]}'
    assert cache_key in edge.values

    # Prove a different colo can refill its local edge from persistent KV.
    edge.values.clear()
    response = asyncio.run(namespace["repository_metadata_cache_get"](
        env, cache_key))
    assert response.body == '{"ok":true,"entries":[]}'
    assert response.headers["cache-control"].startswith("no-store")
    assert cache_key in edge.values


def test_registration_is_signed_account_bound_and_manifest_verified():
    handler = _function_source("https_mirror_endpoint_handler")
    assert "_claimed_node_signing_pubkeys" in handler
    claimed_keys = _function_source("_claimed_node_signing_pubkeys")
    assert "_owner_signing_pubkeys" in claimed_keys
    assert "SELECT pubkey FROM nodes WHERE node_bi=? AND user_bi IS NOT NULL" in claimed_keys
    assert "ed25519_verify" in handler
    assert "_https_mirror_manifest_ok" in handler
    assert "_https_mirror_cloudflare_dns_ok" in handler
    assert "_https_mirror_public_dns_ok" in handler
    assert "_https_mirror_direct_health_ok" in handler
    assert 'registration["mirrorUrl"]' in handler
    assert "cloudflare_proxied_dns_required" in handler
    assert "mirror_https_endpoints" in handler
    assert "stale_registration" in handler
    assert "_https_mirror_refresh_registered_health" in handler
    assert '"health": "active" if health_active else "pending"' in handler
    assert "repositoryBytesInD1" in handler
    assert 'cache_control="no-store"' in handler
    assert "private" not in handler.lower()

    manifest = _function_source("_https_mirror_manifest_ok")
    assert "/forkmesh-mirror.json" in manifest
    assert "validate_tunnel_manifest" in manifest
    assert "ed25519_verify" in manifest

    durable_write = handler.index(
        '"""INSERT INTO mirror_https_endpoints')
    activation = handler.index(
        "await _https_mirror_refresh_registered_health")
    assert durable_write < activation


def test_unchanged_fresh_endpoint_renewal_is_a_zero_write_fast_path():
    namespace = {"HTTPS_MIRROR_ENDPOINT_FAST_RENEW_MS": 5 * 60_000}
    exec(_function_source("_https_mirror_endpoint_renewal_state"), namespace)
    classify = namespace["_https_mirror_endpoint_renewal_state"]
    now = 1_700_000_000_000
    registration = {
        "baseUrl": "https://mirror9.forkmesh.com",
        "publicKey": "public-key",
    }
    existing = {
        "base_url": registration["baseUrl"],
        "public_key": registration["publicKey"],
        "checked_at": now - 5 * 60_000,
        "forkmesh_verified_at": now - 5 * 60_000,
        "healthy": 1,
        "integrity": "ok",
        "abuse_blocked": 0,
        "forkmesh_active": 1,
    }
    assert classify(existing, registration, now) == "fresh"
    assert classify(
        {**existing, "checked_at": now - 5 * 60_000 - 1},
        registration,
        now,
    ) == "stale"
    for field, value in (
        ("healthy", 0),
        ("integrity", "degraded"),
        ("abuse_blocked", 1),
        ("forkmesh_active", 0),
    ):
        assert classify({**existing, field: value}, registration, now) == "stale"
    assert classify(
        {**existing, "base_url": "https://other.example"}, registration, now
    ) == "changed"
    assert classify(
        {**existing, "public_key": "rotated-key"}, registration, now
    ) == "changed"

    handler = _function_source("https_mirror_endpoint_handler")
    fast_return = handler.index('if renewal_state == "fresh"')
    accepted_refs = handler.index("_https_mirror_accepted_forkmesh_refs")
    durable_write = handler.index('"""INSERT INTO mirror_https_endpoints')
    assert fast_return < accepted_refs < durable_write
    assert 'if renewal_state == "changed"' in handler


def test_mainnode_router_identity_readiness_proves_the_seed_matches_public_key():
    calls = []

    async def sign(public_key, signing_seed, challenge):
        calls.append(("sign", public_key, signing_seed, challenge))
        return signing_seed

    async def verify(public_key, signature, challenge):
        calls.append(("verify", public_key, signature, challenge))
        return public_key == signature

    namespace = {
        "_https_mirror_router_public_key": lambda env: env.public_key,
        "_https_mirror_router_seed": lambda env: env.signing_seed,
        "HTTPS_MIRROR_ROUTER_READINESS_CHALLENGE": (
            b"forkmesh-mirror-router-readiness-v1\nmainnode"
        ),
        "ed25519_sign": sign,
        "ed25519_verify": verify,
    }
    exec(_function_source("_https_mirror_router_identity_status"), namespace)
    status = namespace["_https_mirror_router_identity_status"]

    matching = asyncio.run(status(SimpleNamespace(
        public_key="matched-pair", signing_seed="matched-pair"
    )))
    assert matching == ("matched-pair", True)
    assert calls[0][3] == calls[1][3] == (
        b"forkmesh-mirror-router-readiness-v1\nmainnode"
    )

    mismatched = asyncio.run(status(SimpleNamespace(
        public_key="public", signing_seed="different-seed"
    )))
    assert mismatched == ("public", False)

    calls_before_missing = len(calls)
    missing = asyncio.run(status(SimpleNamespace(
        public_key="public", signing_seed=""
    )))
    assert missing == ("public", False)
    assert len(calls) == calls_before_missing


def test_dns_over_https_requests_cloudflare_json_media_type(monkeypatch):
    calls = []

    class Response:
        status = 200
        headers = {}

        async def text(self):
            return '{"Status":0,"Answer":[]}'

    async def fetch(url, options):
        calls.append((url, options))
        return Response()

    async def fetch_with_timeout(url, options, timeout):
        assert timeout == 5
        return await fetch(url, options)

    namespace = {
        "js_fetch_with_timeout": fetch_with_timeout,
    }
    exec(_function_source("_https_mirror_fetch_text"), namespace)
    result = asyncio.run(namespace["_https_mirror_fetch_text"](
        "https://cloudflare-dns.com/dns-query?name=mirror.example&type=A",
        4096,
        5,
        "application/dns-json",
    ))
    assert result == (200, '{"Status":0,"Answer":[]}')
    assert calls[0][1]["headers"]["accept"] == "application/dns-json"

    dns_check = _function_source("_https_mirror_cloudflare_dns_ok")
    assert '"application/dns-json"' in dns_check


def test_cron_verifies_fresh_forkmesh_proof_and_clears_failed_state():
    scheduled = _function_source("_cron_due_phases")
    phase_runner = _function_source("_run_cron_phase")
    health = _function_source("_https_mirror_health_one")
    cron = _function_source("https_mirror_health_cron")
    failed = _function_source("_https_mirror_mark_failed")
    assert '"mirror-health"' in scheduled
    assert (
        'if phase == "mirror-health":\n'
        '        return await https_mirror_health_cron(env)'
    ) in phase_runner
    assert "repository_health_challenge" in health
    assert "_https_mirror_cloudflare_dns_ok" in health
    assert "messageSha256" in health
    assert "ed25519_verify" in health
    assert "_https_mirror_refs_match" in health
    assert "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS" in health
    assert "return forkmesh_active" in health
    assert "_https_mirror_accepted_forkmesh_refs" in cron
    assert "asyncio.gather" not in cron
    assert "for row in rows" in cron
    assert "forkmesh_active=0" in failed
    assert "healthy=0" in failed


def test_flagship_state_pin_resolves_org_alias_but_health_challenges_public_alias():
    namespace = {"re": re}
    exec(_function_source("_https_mirror_expected_forkmesh_refs"), namespace)
    looked_up = []
    state_hash = "a" * 64

    async def org_repo_node(env, owner, repo):
        assert (owner, repo) == ("forkmesh", "forkmesh")
        return "mirror2"

    async def blind_index(env, value):
        looked_up.append(value)
        return "repo-bi"

    async def d1_first(env, sql, *params):
        assert "FROM repositories" in sql
        assert params == ("repo-bi",)
        return {"data": "encrypted", "is_private": 0}

    async def decrypt_row(env, value):
        assert value == "encrypted"
        return {
            "owner": "mirror2",
            "name": "forkmesh",
            "visibility": "public",
            "stateHash": state_hash,
        }

    namespace.update({
        "_org_repo_node": org_repo_node,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    })
    result = asyncio.run(
        namespace["_https_mirror_expected_forkmesh_refs"](object()))
    assert result == state_hash
    assert looked_up == ["mirror2/forkmesh"]

    health = _function_source("_https_mirror_health_one")
    assert '"&owner=forkmesh&repo=forkmesh"' in health


def test_flagship_state_pin_falls_back_to_account_namespace_without_org_alias():
    namespace = {"re": re}
    exec(_function_source("_https_mirror_expected_forkmesh_refs"), namespace)
    looked_up = []
    state_hash = "b" * 64

    async def org_repo_node(env, owner, repo):
        return ""

    async def blind_index(env, value):
        looked_up.append(value)
        return "repo-bi"

    async def d1_first(env, sql, *params):
        return {"data": "encrypted", "is_private": 0}

    async def decrypt_row(env, value):
        return {"visibility": "public", "stateHash": state_hash}

    namespace.update({
        "_org_repo_node": org_repo_node,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    })
    result = asyncio.run(
        namespace["_https_mirror_expected_forkmesh_refs"](object()))
    assert result == state_hash
    assert looked_up == ["forkmesh/forkmesh"]


def test_flagship_health_accepts_recent_source_pin_during_mirror_convergence():
    namespace = {
        "re": re,
        "hmac": __import__("hmac"),
    }
    exec(_function_source("_https_mirror_refs_match"), namespace)
    matches = namespace["_https_mirror_refs_match"]
    current = "c" * 64
    previous = "b" * 64

    assert matches(current, frozenset({current, previous})) is True
    assert matches(previous, frozenset({current, previous})) is True
    assert matches("a" * 64, frozenset({current, previous})) is False
    assert matches("not-a-digest", frozenset({current, previous})) is False


def test_flagship_quorum_requires_two_fresh_group_mirrors():
    now = 1_700_000_000_000
    required = {"git-info-refs", "git-upload-pack", "tree", "blob", "raw"}
    agreed = "a" * 64
    rows = [
        {
            "node_name": "mirror9",
            "checked_at": now,
            "forkmesh_refs_sha256": agreed,
            "forkmesh_operations_json": json.dumps(sorted(required)),
        },
        {
            "node_name": "mirror10",
            "checked_at": now,
            "forkmesh_refs_sha256": agreed,
            "forkmesh_operations_json": json.dumps(sorted(required)),
        },
        {
            "node_name": "outside",
            "checked_at": now,
            "forkmesh_refs_sha256": "b" * 64,
            "forkmesh_operations_json": json.dumps(sorted(required)),
        },
    ]

    async def d1_all(_env, sql, *params):
        assert "healthy=1" in sql
        assert params == (now - 600_000,)
        return rows

    namespace = {
        "Date": type("D", (), {"now": staticmethod(lambda: now)}),
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "d1_all": d1_all,
        "https_routing": SimpleNamespace(ENDPOINT_STALE_MS=600_000),
        "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS": required,
        "json": json,
        "re": re,
    }
    exec(_function_source("_https_mirror_flagship_quorum"), namespace)
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    result = asyncio.run(namespace["_https_mirror_flagship_quorum"](
        object(), {"mirror9", "mirror10", "outside"}, now=now))
    assert result["pins"] == frozenset({agreed})
    assert result["nodes"] == frozenset({"mirror9", "mirror10"})

    # Removing either agreeing member dissolves the quorum; an outsider cannot
    # take its place unless it is explicitly in this repository's mirror group.
    result = asyncio.run(namespace["_https_mirror_flagship_quorum"](
        object(), {"mirror9"}, now=now))
    assert result["pins"] == frozenset()


def _source_dark_group_namespace(
        *, source_state, mirror_state, endpoints, address="mirror11",
        linked=False, override=False, source_kind="local-node",
        include_local_source=False, owner_merge=None, mirror_states=None):
    """Run the real routing-context builder over one simulated mirror group.

    `address` is the /<owner>/forkmesh the visitor actually requested, so a
    case can ask for a mirror's own catalog row rather than the source's.
    """
    now = 1_700_000_000_000
    required = set(_REQUIRED_FORKMESH_OPERATIONS)
    queries = []

    def _record(owner, state, source):
        return {
            "owner": owner,
            "name": "forkmesh",
            "machineName": owner,
            "visibility": "public",
            "rootCommit": "root",
            "source": source,
            "stateHash": state,
        }

    catalog = [
        {
            "key_bi": "key-mirror11",
            "is_private": 0,
            "data": _record("mirror11", source_state, source_kind),
        },
    ] + [
        {
            "key_bi": "key-%s" % node,
            "is_private": 0,
            "data": _record(
                node, (mirror_states or {}).get(node, mirror_state),
                "remote-clone"),
        }
        for node in ("mirror9", "mirror14")
    ]
    if include_local_source:
        catalog.append({
            "key_bi": "key-source-node",
            "is_private": 0,
            "data": _record("source-node", source_state, "local-node"),
        })

    async def d1_all(_env, sql, *params):
        queries.append(sql)
        if "mirror_https_endpoints" in sql:
            return [
                {
                    "node_name": node,
                    "checked_at": now,
                    "forkmesh_refs_sha256": digest,
                    "forkmesh_operations_json": json.dumps(sorted(required)),
                }
                for node, digest in endpoints
            ]
        return []

    async def d1_first(_env, _sql, *_params):
        if linked and "org_repos" in _sql:
            return {"linked": 1}
        if owner_merge is not None and "repo_merge_jobs" in _sql:
            return {"result": json.dumps(dict(
                {"ok": True, "status": "merged", "published": True},
                **owner_merge))}
        return None

    async def admission_override_active(*_args, **_kwargs):
        return override

    async def catalog_rows(_env, _now):
        return catalog

    namespace = _public_context_cache_namespace()
    namespace.update({
        "Date": type("D", (), {"now": staticmethod(lambda: now)}),
        "MAX_NODE_NAME": 80,
        "MAX_REPO_SEGMENT": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "safe_segment": lambda value: str(value or "").strip().lower(),
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "re": re,
        "json": json,
        "https_routing": SimpleNamespace(ENDPOINT_STALE_MS=600_000),
        "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS": required,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "ensure_schema": _noop_async,
        "_decrypted_public_catalog": catalog_rows,
        "_is_blocked_catalog_identity": lambda _env, _owner, _name: False,
        "_hosted_repository_import_route": _none_async,
        "_https_mirror_history_keys": lambda members, key, **kwargs: [key],
        "_bounded_repo_state_history": _empty_history,
        "_admission_override_active": admission_override_active,
        "admission_override_pins": mirror_policy.admission_override_pins,
        "clone_state_pins": mirror_policy.clone_state_pins,
        "peer_quorum_pins": mirror_policy.peer_quorum_pins,
        "repo_mirror_same_group": mirror_policy.repo_mirror_same_group,
        "decrypt_row": _identity_async,
        "blind_index": _blind_index_async,
    })
    namespace["hmac"] = hmac
    exec(_function_source("_https_mirror_flagship_quorum"), namespace)
    # An owner's web merge can override the current generation; with no
    # succeeded merge job the override is inert and the classic rules decide.
    exec(_function_source("_owner_merge_pin_state"), namespace)
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    exec(_function_source("_https_mirror_public_context"), namespace)
    context = asyncio.run(namespace["_https_mirror_public_context"](
        object(), address, "forkmesh"))
    return context, queries


_REQUIRED_FORKMESH_OPERATIONS = (
    "git-info-refs", "git-upload-pack", "tree", "blob", "raw",
)


async def _noop_async(*_args, **_kwargs):
    return None


async def _none_async(*_args, **_kwargs):
    return None


async def _false_async(*_args, **_kwargs):
    return False


async def _empty_history(*_args, **_kwargs):
    return {}


async def _identity_async(_env, value):
    return value


async def _blind_index_async(_env, value):
    return "bi:" + str(value)


def test_agreeing_mirrors_serve_a_group_whose_source_of_truth_went_dark():
    # The regression: a group's only working-copy holder stops publishing (a
    # retired node), so every healthy mirror falls out of its pin window and the
    # whole repository answers 503 "mirror_unavailable" with no way back — a
    # mirror's catch-up fetch is the same gated clone route being refused.
    dead = "1" * 64
    live = "2" * 64
    context, queries = _source_dark_group_namespace(
        source_state=dead,
        mirror_state=live,
        endpoints=(("mirror9", live), ("mirror14", live)),
    )
    assert context is not None
    assert context["nodes"] == {"mirror11", "mirror9", "mirror14"}
    assert {"mirror9", "mirror14"}.issubset(context["currentNodes"])
    assert live in context["pins"]
    assert live in context["currentPins"]
    # The source's own attestation is never dropped: it may serve again the
    # moment it comes back.
    assert dead in context["pins"]

    # One node is not a quorum. A state only ONE node has ever published is a
    # claim, not agreement — a tampered mirror can always publish a hash that
    # matches its own forged refs — so it cannot repin the route and the
    # repository keeps failing closed on it.
    forged = "9" * 64
    context, _ = _source_dark_group_namespace(
        source_state=dead,
        mirror_state=live,
        mirror_states={"mirror9": forged},
        endpoints=(("mirror9", forged), ("mirror14", live)),
    )
    assert forged not in context["pins"]
    assert "mirror9" not in context["nodes"]

    # Nor do two nodes that disagree: those are two unsupported claims, not two
    # votes, so nothing new is admitted and the source's window still stands
    # alone.
    context, _ = _source_dark_group_namespace(
        source_state=dead,
        mirror_state=live,
        mirror_states={"mirror9": live, "mirror14": "8" * 64},
        endpoints=(("mirror9", live), ("mirror14", "8" * 64)),
    )
    assert context["pins"] == {dead}
    assert context["nodes"] == {"mirror11"}


def test_catalog_agreement_carries_a_repo_no_signed_proof_can_vouch_for():
    # The signed endpoint proof covers the flagship repository only, so for an
    # ordinary user's repo the endpoint quorum can never match its state and the
    # group stayed dark for good once its desktop stopped publishing. Two nodes
    # that independently published the same state are the same two-distinct-
    # nodes evidence, and it is available for every repo.
    dead = "1" * 64
    live = "2" * 64
    flagship = "f" * 64          # what the endpoints actually attest
    context, _ = _source_dark_group_namespace(
        source_state=dead,
        mirror_state=live,
        endpoints=(("mirror9", flagship), ("mirror14", flagship)),
    )
    assert context is not None
    assert live in context["pins"]
    assert dead in context["pins"]
    assert {"mirror9", "mirror14"}.issubset(context["nodes"])


def test_owner_web_merge_makes_the_merged_mirror_current_while_source_is_dark():
    # An organization owner merged a pull request through mirror9 while the
    # source-of-truth desktop was offline. mirror9 now serves a state the
    # source never attested; mirror14 still serves the source's last state.
    # The durable merge job (mirror-attested state + the source state it was
    # recorded against) makes the merged generation current, so mirror9 is
    # routable and preferred and mirror14's next relay fetch can reach it.
    source = "1" * 64
    merged = "2" * 64
    context, _ = _source_dark_group_namespace(
        source_state=source,
        mirror_state=source,
        endpoints=(("mirror9", merged), ("mirror14", source)),
        owner_merge={"state": merged, "sourceState": source},
    )
    assert context is not None
    assert {"mirror9", "mirror14", "mirror11"} == context["nodes"]
    assert "mirror9" in context["currentNodes"]
    assert merged in context["pins"] and merged in context["currentPins"]
    assert context["ownerMergeState"] == merged
    assert context["sourceState"] == source
    # The source's own attestation is still admitted: it may serve again the
    # moment it comes back, and its next publish retires the override.
    assert source in context["pins"]

    # Without the owner's merge job the very same mirror is a lone divergent
    # endpoint and stays outside the pin set exactly as before.
    context, _ = _source_dark_group_namespace(
        source_state=source,
        mirror_state=source,
        endpoints=(("mirror9", merged), ("mirror14", source)),
    )
    assert context["ownerMergeState"] == ""
    assert merged not in context["pins"]

    # Once the source publishes a different state (it converged onto the
    # merge, or moved on), the job no longer describes the current generation
    # and the override is inert.
    context, _ = _source_dark_group_namespace(
        source_state="3" * 64,
        mirror_state="3" * 64,
        endpoints=(("mirror9", merged), ("mirror14", "3" * 64)),
        owner_merge={"state": merged, "sourceState": source},
    )
    assert context["ownerMergeState"] == ""
    assert merged not in context["currentPins"]


def test_signed_override_recovers_divergent_linked_mirror_endpoints():
    dead = "1" * 64
    first = "2" * 64
    second = "3" * 64
    context, _ = _source_dark_group_namespace(
        source_state=dead,
        mirror_state="4" * 64,
        endpoints=(("mirror9", first), ("mirror14", second)),
        linked=True,
        override=True,
    )
    assert context is not None
    assert {"mirror9", "mirror14"}.issubset(context["nodes"])
    assert {first, second}.issubset(context["pins"])
    assert {first, second}.issubset(context["currentPins"])
    assert {"mirror9", "mirror14"}.issubset(context["currentNodes"])


def test_signed_override_recovers_group_with_only_retired_mirror_authority():
    first = "6" * 64
    second = "7" * 64
    context, _ = _source_dark_group_namespace(
        source_state="1" * 64,
        mirror_state="4" * 64,
        endpoints=(("mirror9", first), ("mirror14", second)),
        override=True,
        source_kind="remote-clone",
    )
    assert context is not None
    assert {"mirror9", "mirror14"}.issubset(context["nodes"])
    assert {first, second}.issubset(context["currentPins"])


def test_signed_override_recovers_retired_target_with_stale_local_source():
    first = "8" * 64
    second = "9" * 64
    context, _ = _source_dark_group_namespace(
        source_state="1" * 64,
        mirror_state="4" * 64,
        endpoints=(("mirror9", first), ("mirror14", second)),
        override=True,
        source_kind="remote-clone",
        include_local_source=True,
    )
    assert {"mirror9", "mirror14"}.issubset(context["nodes"])
    assert {first, second}.issubset(context["currentPins"])


def test_linked_override_is_not_needed_when_strict_endpoint_is_available():
    state = "5" * 64
    context, _ = _source_dark_group_namespace(
        source_state=state,
        mirror_state=state,
        endpoints=(("mirror9", state),),
        linked=True,
        override=False,
    )
    assert context is not None
    assert context["nodes"] == {"mirror11", "mirror9", "mirror14"}


def test_a_mirrors_own_address_routes_as_the_repository_it_copies():
    # A mirror publishes its copy into the public catalog under its own
    # account, so /<mirror>/<repo> is a browsable address. Only that one node
    # ever serves that path (mirror_gateway serves exactly the owner/name
    # pairs in its config), so the routing identity must resolve to the
    # repository rather than the copy — otherwise the address dies with the
    # node while healthy mirrors of the same repository keep serving. Pinned
    # here because the browser side now depends on it: repoApiBase addresses
    # a repo by its public owner, which lands on this path.
    dead = "1" * 64
    live = "2" * 64

    context, _ = _source_dark_group_namespace(
        source_state=live,
        mirror_state=live,
        endpoints=(("mirror9", live), ("mirror14", live)),
        address="mirror9",
    )
    assert context["owner"] == "mirror11"

    # Still true once the source of truth goes dark and the quorum branch
    # admits the mirrors instead: the identity is the repository, the admitted
    # serving set is the mirrors.
    context, _ = _source_dark_group_namespace(
        source_state=dead,
        mirror_state=live,
        endpoints=(("mirror9", live), ("mirror14", live)),
        address="mirror9",
    )
    assert context is not None
    assert context["owner"] == "mirror11"
    assert context["repo"] == "forkmesh"
    assert {"mirror9", "mirror14"}.issubset(context["nodes"])


def test_reachable_source_pin_window_still_owns_routing_and_costs_no_query():
    # Control: while any mirror is inside the source's window the source of
    # truth remains the sole authority, and the recovery read is never issued.
    state = "3" * 64
    context, queries = _source_dark_group_namespace(
        source_state=state,
        mirror_state=state,
        endpoints=(("mirror9", "4" * 64), ("mirror14", "4" * 64)),
    )
    assert context is not None
    assert context["nodes"] == {"mirror11", "mirror9", "mirror14"}
    assert context["pins"] == {state}
    assert not any("mirror_https_endpoints" in sql for sql in queries)


def test_flagship_candidate_ring_survives_mirror_add_and_remove():
    now = 1_700_000_000_000
    state = {"cursor": 0, "nodes": ["mirror2", "mirror9", "mirror10"]}

    def endpoint(node, latency):
        return {
            "node_name": node,
            "base_url": "https://%s.example.test" % node,
            "public_key": "public-key",
            "registration_sig": "registration",
            "issued_at": 1,
            "checked_at": now,
            "latency_ms": latency,
            "region": "US",
            "healthy": 1,
            "integrity": "ok",
            "abuse_blocked": 0,
            "forkmesh_refs_sha256": "a" * 64,
        }

    latencies = {"mirror2": 10, "mirror9": 20, "mirror10": 30, "mirror13": 40}

    async def d1_all(_env, _sql, *_params):
        return [endpoint(node, latencies[node]) for node in state["nodes"]]

    namespace = {
        "d1_all": d1_all,
        "_random_bytes": lambda size: state["cursor"].to_bytes(size, "big"),
        "Date": type("D", (), {"now": staticmethod(lambda: now)}),
        "https_routing": routing,
    }
    exec(_function_source("_https_mirror_endpoint_projection"), namespace)
    _load_avoid_helpers(namespace)
    exec(_function_source("_https_mirror_candidates"), namespace)
    candidates = namespace["_https_mirror_candidates"]

    context = {
        "repoBi": "flagship-bi",
        "nodes": set(state["nodes"]),
        "roundRobinAll": True,
    }

    served = []
    for cursor in range(3):
        state["cursor"] = cursor
        served.append(asyncio.run(candidates(
            object(), context, "US"))[0]["node"])
    assert served == ["mirror2", "mirror9", "mirror10"]

    state["nodes"] = ["mirror9", "mirror10"]
    context["nodes"] = set(state["nodes"])
    state["cursor"] = 5  # stale cursor from the larger ring
    assert asyncio.run(candidates(
        object(), context, "US"))[0]["node"] == "mirror10"

    state["nodes"].append("mirror13")
    context["nodes"] = set(state["nodes"])
    state["cursor"] = 2
    assert asyncio.run(candidates(
        object(), context, "US"))[0]["node"] == "mirror13"


def _merge_group_candidates(writes=("merge-pull", "update-pull-branch")):
    """Run the real candidate builder over one six-mirror group."""
    now = 1_700_000_000_000
    state = {"cursor": 0}
    refs = "a" * 64
    reads = ["blob", "git-info-refs", "git-upload-pack", "raw", "tree"]
    nodes = ["mirror%d" % index for index in range(1, 7)]
    # Only the last mirror in read order runs a merge executor, which is the
    # ordinary shape of a group: every other node is read-only.
    writes = list(writes)
    operations = {
        node: (reads + writes if node == "mirror6" else reads)
        for node in nodes
    }

    async def d1_all(_env, _sql, *_params):
        return [
            {
                "node_name": node,
                "base_url": "https://%s.example.test" % node,
                "public_key": "public-key",
                "registration_sig": "registration",
                "issued_at": 1,
                "checked_at": now,
                "latency_ms": (index + 1) * 10,
                "region": "US",
                "healthy": 1,
                "integrity": "ok",
                "abuse_blocked": 0,
                "forkmesh_refs_sha256": refs,
                "forkmesh_operations_json": json.dumps(
                    sorted(operations[node])),
            }
            for index, node in enumerate(nodes)
        ]

    namespace = {
        "d1_all": d1_all,
        "json": json,
        "_random_bytes": lambda size: state["cursor"].to_bytes(size, "big"),
        "Date": type("D", (), {"now": staticmethod(lambda: now)}),
        "https_routing": routing,
    }
    exec(_function_source("_https_mirror_endpoint_projection"), namespace)
    exec(
        _function_source("_https_mirror_advertised_operation_nodes"),
        namespace,
    )
    _load_avoid_helpers(namespace)
    exec(_function_source("_https_mirror_candidates"), namespace)
    context = {
        "repoBi": "group-bi",
        "nodes": set(nodes),
        "currentNodes": {"mirror1"},
        "currentPins": {refs},
    }

    def served(cursor, **kwargs):
        state["cursor"] = cursor
        return [
            candidate["node"] for candidate in asyncio.run(
                namespace["_https_mirror_candidates"](
                    object(), context, "US", **kwargs))
        ]

    return served, nodes


def test_a_web_merge_reaches_the_one_mirror_that_runs_the_executor():
    # The reported failure: an owner merging on the web read "no online mirror
    # can merge this pull request right now" while the mirror that runs the
    # executor was healthy, current, and serving reads. Selection rotated a
    # read window over the group and only then asked each candidate for the
    # merge capability, so the single capable node usually never made the cut.
    served, nodes = _merge_group_candidates()
    assert "mirror6" not in served(0)
    for cursor in range(len(nodes)):
        selected = served(cursor, operation="merge-pull")
        assert selected[0] == "mirror6"
        assert len(selected) == routing.MAX_FAILOVER_ATTEMPTS
        assert len(set(selected)) == len(selected)
    # The other owner-authorized mutation is routed the same way, and a read
    # never consults the advertisement: its ring is unchanged.
    assert served(0, operation="update-pull-branch")[0] == "mirror6"
    assert served(0, operation="blob") == served(0)


def test_an_in_flight_merge_job_keeps_reaching_its_recorded_node():
    # Every poll of a 202 merge job re-selects candidates and then looks up the
    # node the job recorded. Sorting the pinned node to the front of a window
    # it had already been truncated out of failed that lookup mid-merge, which
    # the browser reports as the same merge_node_unavailable error.
    served, nodes = _merge_group_candidates()
    for cursor in range(len(nodes)):
        selected = served(cursor, sticky="mirror6")
        assert selected[0] == "mirror6"
        assert len(selected) == routing.MAX_FAILOVER_ATTEMPTS
    # A clone advertisement pins its node the same way.
    assert served(3, sticky="mirror1")[0] == "mirror1"
    # Ordinary reads keep their randomized ring untouched.
    assert served(1) == ["mirror2", "mirror3", "mirror4", "mirror5"]


def test_flagship_accepted_pins_include_bounded_source_history():
    namespace = {
        "re": re,
        "STATE_PIN_HISTORY": 100,
        "Date": type("Clock", (), {"now": staticmethod(lambda: 1_000)}),
        "_HTTPS_MIRROR_STATE_PIN_MEMO": {
            "key_bi": None, "max_ts": None, "ts": 0, "pins": frozenset()},
        "HTTPS_MIRROR_STATE_PIN_MEMO_MAX_AGE_MS": 600_000,
    }
    exec(
        _function_source("_https_mirror_accepted_forkmesh_refs"),
        namespace,
    )
    exec(_function_source("_https_mirror_state_history_pins"), namespace)
    current = "d" * 64
    previous = "c" * 64
    queries = []

    async def org_repo_node(env, owner, repo):
        assert (owner, repo) == ("forkmesh", "forkmesh")
        return "source-node"

    async def blind_index(env, value):
        assert value == "source-node/forkmesh"
        return "repo-bi"

    async def d1_first(env, sql, *params):
        queries.append((sql, params))
        return {"data": "encrypted", "is_private": 0}

    async def d1_all(env, sql, *params):
        queries.append((sql, params))
        return [
            {"state_hash": previous},
            {"state_hash": "invalid"},
        ]

    async def decrypt_row(env, value):
        assert value == "encrypted"
        return {
            "visibility": "public",
            "stateHash": current,
        }

    namespace.update({
        "_org_repo_node": org_repo_node,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 80,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
    })
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    pins = asyncio.run(
        namespace["_https_mirror_accepted_forkmesh_refs"](object()))

    assert pins == frozenset({current, previous})
    # One newest-ts probe fingerprints the history before the bounded read.
    assert "MAX(ts)" in queries[1][0]
    assert queries[1][1] == ("repo-bi",)
    assert queries[2][1] == ("repo-bi", 100)


def test_flagship_accepted_pins_include_deployed_revision_during_catalog_failure():
    deployed = "e" * 64

    async def org_repo_node(*_args):
        raise RuntimeError("temporary catalog failure")

    namespace = {
        "re": re,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 80,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "_org_repo_node": org_repo_node,
    }
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    exec(_function_source("_https_mirror_accepted_forkmesh_refs"), namespace)
    env = type("Env", (), {
        "FLAGSHIP_ROUTING_REFS_SHA256": deployed,
    })()

    pins = asyncio.run(
        namespace["_https_mirror_accepted_forkmesh_refs"](env))

    assert pins == frozenset({deployed})


def test_deployed_flagship_pin_is_scoped_to_the_public_flagship_route():
    namespace = {
        "re": re,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    deployed_refs = namespace["_https_mirror_deployed_flagship_refs"]
    digest = "f" * 64
    env = type("Env", (), {
        "FLAGSHIP_ROUTING_REFS_SHA256": digest,
    })()

    assert deployed_refs(env) == frozenset({digest})
    assert deployed_refs(env, "forkmesh", "other") == frozenset()
    assert deployed_refs(env, "other", "forkmesh") == frozenset()

    context = _function_source("_https_mirror_public_context")
    addition = context.index("deployed_pins =")
    allowed = context.index("allowed = set()")
    assert addition < allowed
    assert "pins.update(deployed_pins)" in context
    assert "current_pins.update(deployed_pins)" in context


def test_fresh_endpoint_lease_rechecks_an_advanced_catalog_generation():
    current = "a" * 64

    async def blind_index(_env, value):
        assert value == "mirror32/forkmesh"
        return "repo-bi"

    async def d1_first(_env, sql, *args):
        assert "FROM repositories" in sql
        assert args == ("repo-bi",)
        return {"data": "sealed", "is_private": 0}

    async def decrypt_row(_env, value):
        assert value == "sealed"
        return {
            "owner": "mirror32",
            "name": "forkmesh",
            "visibility": "public",
            "stateHash": current,
        }

    namespace = {
        "re": re,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
    }
    exec(_function_source("_https_mirror_published_forkmesh_refs"), namespace)
    assert asyncio.run(
        namespace["_https_mirror_published_forkmesh_refs"](
            object(), "mirror32")) == current

    handler = _function_source("https_mirror_endpoint_handler")
    assert "forkmesh_refs_sha256" in handler
    check = handler.index("_https_mirror_published_forkmesh_refs")
    fast_return = handler.index("return await registration_response(True)")
    accepted_snapshot = handler.index("accepted_before_registration")
    assert check < fast_return < accepted_snapshot
    assert "hmac.compare_digest(published_refs, leased_refs)" in handler


def test_flagship_recovery_follows_newest_node_owned_by_configured_anchor():
    async def blind_index(_env, value):
        assert value == "mirror29"
        return "anchor-bi"

    async def d1_first(_env, sql, *args):
        assert "SELECT user_bi FROM nodes" in sql
        assert args == ("anchor-bi",)
        return {"user_bi": "operator-bi"}

    async def d1_all(_env, sql, *args):
        assert "JOIN nodes" in sql
        assert args == ("operator-bi",)
        return [
            {"node_name": "mirror29", "issued_at": 100},
            {"node_name": "mirror34", "issued_at": 300},
            {"node_name": "unrelated", "issued_at": 900},
        ]

    namespace = {
        "re": re,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "blind_index": blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
    }
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    recovery_refs = namespace["_https_mirror_flagship_recovery_refs"]
    current = "a" * 64
    divergent = "b" * 64
    outside_group = "c" * 64
    quorum = {
        "nodeDigests": {
            "mirror29": current,
            "mirror34": divergent,
            # The ownership query alone is insufficient: recovery remains
            # bounded by the already validated repository group proof set.
            "other-group-node": outside_group,
        },
    }

    env = type("Env", (), {"FLAGSHIP_RECOVERY_NODE": "mirror29"})()
    selected = asyncio.run(recovery_refs(env, quorum))
    assert selected == frozenset({divergent})
    assert current not in selected
    assert outside_group not in selected

    invalid = type("Env", (), {"FLAGSHIP_RECOVERY_NODE": "../mirror29"})()
    assert asyncio.run(recovery_refs(invalid, quorum)) == frozenset()


def test_flagship_recovery_without_account_binding_keeps_exact_anchor():
    async def blind_index(_env, _value):
        return "anchor-bi"

    async def d1_first(_env, _sql, *_args):
        return {"user_bi": ""}

    namespace = {
        "re": re,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "blind_index": blind_index,
        "d1_first": d1_first,
    }
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    env = type("Env", (), {"FLAGSHIP_RECOVERY_NODE": "mirror29"})()
    selected = asyncio.run(namespace["_https_mirror_flagship_recovery_refs"](
        env,
        {"nodeDigests": {"mirror29": "a" * 64, "mirror34": "b" * 64}},
    ))
    assert selected == frozenset({"a" * 64})


def test_flagship_override_renews_fresh_divergent_endpoint_generations():
    current = "a" * 64
    first = "b" * 64
    second = "c" * 64
    override = {"active": False}

    async def d1_first(_env, _sql, *_params):
        return {"data": "encrypted", "is_private": 0}

    async def d1_all(_env, _sql, *_params):
        return []

    async def org_repo_node(*_args):
        return "source-node"

    async def repo_blind_index(*_args):
        return "repo-bi"

    async def decrypt_record(_env, value):
        assert value == "encrypted"
        return {
            "owner": "source-node",
            "name": "forkmesh",
            "visibility": "public",
            "stateHash": current,
        }

    async def catalog_rows(_env, _now):
        return [{
            "data": {
                "owner": "source-node",
                "name": "forkmesh",
                "visibility": "public",
                "machineName": "source-node",
                "stateHash": current,
            },
        }, {
            "data": {
                "owner": "mirror9",
                "name": "forkmesh",
                "visibility": "public",
                "machineName": "mirror9",
                "stateHash": "d" * 64,
            },
        }]

    async def quorum(_env, allowed_nodes):
        assert allowed_nodes == {"source-node", "mirror9"}
        return {
            "pins": frozenset(),
            "nodes": frozenset(),
            "nodeDigests": {"mirror9": first, "source-node": second},
        }

    async def override_active(_env, repo_bi):
        assert repo_bi == "repo-bi"
        return override["active"]

    namespace = {
        "re": re,
        "STATE_PIN_HISTORY": 100,
        "Date": type("D", (), {"now": staticmethod(lambda: 1)}),
        "MAX_NODE_NAME": 80,
        "_org_repo_node": org_repo_node,
        "blind_index": repo_blind_index,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_record,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "_decrypted_public_catalog": catalog_rows,
        "repo_mirror_same_group": mirror_policy.repo_mirror_same_group,
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(value or ""))),
        "_https_mirror_flagship_quorum": quorum,
        "_admission_override_active": override_active,
        "Date": type("Clock", (), {"now": staticmethod(lambda: 1_000)}),
        "_HTTPS_MIRROR_STATE_PIN_MEMO": {
            "key_bi": None, "max_ts": None, "ts": 0, "pins": frozenset()},
        "HTTPS_MIRROR_STATE_PIN_MEMO_MAX_AGE_MS": 600_000,
    }
    exec(_function_source("_https_mirror_deployed_flagship_refs"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_node"), namespace)
    exec(_function_source("_https_mirror_flagship_recovery_refs"), namespace)
    exec(_function_source("_https_mirror_state_history_pins"), namespace)
    exec(_function_source("_https_mirror_accepted_forkmesh_refs"), namespace)

    pins = asyncio.run(
        namespace["_https_mirror_accepted_forkmesh_refs"](object()))
    assert pins == frozenset({current})

    override["active"] = True
    pins = asyncio.run(
        namespace["_https_mirror_accepted_forkmesh_refs"](object()))
    assert pins == frozenset({current, first, second})


def test_every_registered_mirror_can_refresh_its_exact_signed_health():
    events = []
    endpoint = {
        "node_bi": "mirror-3-bi",
        "node_name": "mirror3",
        "base_url": "https://mirror3.example.test",
        "public_key": "node-public-key",
    }

    async def d1_first(env, sql, *params):
        events.append(("lookup", params))
        assert "FROM mirror_https_endpoints WHERE node_name=?" in sql
        assert params == ("mirror3",)
        return endpoint

    async def accepted_refs(env):
        events.append(("expected",))
        return frozenset({"c" * 64})

    async def health_one(env, row, expected):
        events.append(("health", row, expected))
        return True

    namespace = {
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "d1_first": d1_first,
        "_https_mirror_accepted_forkmesh_refs": accepted_refs,
        "_https_mirror_health_one": health_one,
    }
    exec(
        _function_source("_https_mirror_refresh_registered_health"),
        namespace,
    )
    refreshed = asyncio.run(
        namespace["_https_mirror_refresh_registered_health"](
            object(), "Mirror3"))
    assert refreshed is True
    assert events == [
        ("lookup", ("mirror3",)),
        ("expected",),
        ("health", endpoint, frozenset({"c" * 64})),
    ]


def test_registered_health_refresh_stays_pending_on_missing_pin_or_failure():
    rows = []
    health_calls = []
    expected = ""

    async def d1_first(env, sql, *params):
        rows.append(params)
        return {
            "node_bi": "mirror-3-bi",
            "node_name": "mirror3",
            "base_url": "https://mirror3.example.test",
            "public_key": "node-public-key",
        }

    async def accepted_refs(env):
        return frozenset({expected}) if expected else frozenset()

    async def health_one(env, row, state_hash):
        health_calls.append((row, state_hash))
        raise RuntimeError("bounded health fetch failed")

    namespace = {
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "d1_first": d1_first,
        "_https_mirror_accepted_forkmesh_refs": accepted_refs,
        "_https_mirror_health_one": health_one,
    }
    exec(
        _function_source("_https_mirror_refresh_registered_health"),
        namespace,
    )
    refresh = namespace["_https_mirror_refresh_registered_health"]

    assert asyncio.run(refresh(object(), "not valid")) is False
    assert rows == []
    assert asyncio.run(refresh(object(), "mirror3")) is False
    assert rows == [("mirror3",)]
    assert health_calls == []

    expected = "d" * 64
    assert asyncio.run(refresh(object(), "mirror3")) is False
    assert rows == [("mirror3",), ("mirror3",)]
    assert len(health_calls) == 1


def test_public_flagship_publish_refreshes_only_its_exact_node_health():
    events = []

    async def org_repo_node(env, owner, repo):
        events.append(("canonical", owner, repo))
        return "mirror2"

    async def refresh_registered(env, node):
        events.append(("health", node))
        return True

    namespace = {
        "MAX_REPO_SEGMENT": 128,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "_org_repo_node": org_repo_node,
        "_https_mirror_refresh_registered_health": refresh_registered,
    }
    exec(
        _function_source("_https_mirror_refresh_catalog_publisher_health"),
        namespace,
    )
    refreshed = asyncio.run(
        namespace["_https_mirror_refresh_catalog_publisher_health"](
            object(),
            {
                "owner": "Mirror2",
                "name": "forkmesh",
                "visibility": "public",
            },
        )
    )
    assert refreshed is True
    assert events == [
        ("canonical", "forkmesh", "forkmesh"),
        ("health", "mirror2"),
    ]


def test_catalog_health_activation_is_flagship_public_only_and_best_effort():
    calls = []
    endpoint_available = True

    async def org_repo_node(env, owner, repo):
        return "mirror2"

    async def refresh_registered(env, node):
        calls.append((node,))
        if not endpoint_available:
            return False
        raise RuntimeError("bounded health fetch failed")

    namespace = {
        "MAX_REPO_SEGMENT": 128,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "_org_repo_node": org_repo_node,
        "_https_mirror_refresh_registered_health": refresh_registered,
    }
    exec(
        _function_source("_https_mirror_refresh_catalog_publisher_health"),
        namespace,
    )
    refresh = namespace["_https_mirror_refresh_catalog_publisher_health"]
    for record in (
        {"owner": "mirror2", "name": "forkmesh", "visibility": "private"},
        {"owner": "mirror2", "name": "another-repo", "visibility": "public"},
        {"owner": "not valid", "name": "forkmesh", "visibility": "public"},
        # Same repository name, but this account is not the organization's
        # current canonical backing node and must not trigger a health fetch.
        {"owner": "mirror3", "name": "forkmesh", "visibility": "public"},
    ):
        assert asyncio.run(refresh(object(), record)) is False
    assert calls == []

    endpoint_available = False
    assert asyncio.run(refresh(object(), {
        "owner": "mirror2",
        "name": "forkmesh",
        "visibility": "public",
    })) is False
    assert calls == [("mirror2",)]

    # Once the exact endpoint is selected, a fetch/verifier exception is
    # swallowed so the already accepted catalog publication is not rolled back.
    endpoint_available = True
    assert asyncio.run(refresh(object(), {
        "owner": "mirror2",
        "name": "forkmesh",
        "visibility": "public",
    })) is False
    assert calls == [("mirror2",), ("mirror2",)]


def test_catalog_health_activation_falls_back_only_to_canonical_account():
    looked_up = []

    async def org_repo_node(env, owner, repo):
        return ""

    async def refresh_registered(env, node):
        looked_up.append((node,))
        return False

    namespace = {
        "MAX_REPO_SEGMENT": 128,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: bool(
            re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", value)),
        "_org_repo_node": org_repo_node,
        "_https_mirror_refresh_registered_health": refresh_registered,
    }
    exec(
        _function_source("_https_mirror_refresh_catalog_publisher_health"),
        namespace,
    )
    refresh = namespace["_https_mirror_refresh_catalog_publisher_health"]
    assert asyncio.run(refresh(object(), {
        "owner": "mirror2",
        "name": "forkmesh",
        "visibility": "public",
    })) is False
    assert looked_up == []
    assert asyncio.run(refresh(object(), {
        "owner": "forkmesh",
        "name": "forkmesh",
        "visibility": "public",
    })) is False
    assert looked_up == [("forkmesh",)]


def test_catalog_persists_before_exact_node_health_activation_and_cache_purge():
    handler = _function_source("catalog_handler")
    durable_write = handler.index(
        "await _contribution_write_catalog_state")
    activation = handler.index(
        "await _https_mirror_refresh_catalog_publisher_health")
    cache_purge = handler.index("await purge_catalog_related_caches()")
    assert durable_write < activation < cache_purge

    activation_source = _function_source(
        "_https_mirror_refresh_catalog_publisher_health")
    assert '_org_repo_node(env, "forkmesh", "forkmesh")' in activation_source
    assert "_https_mirror_refresh_registered_health" in activation_source


def test_public_browse_clone_and_release_are_intercepted_before_host_tunnel():
    route = _method_source("Default", "_route")
    assert route.index("HTTPS_MIRROR_ENDPOINT_PATH") < route.index(
        "org_alias_rewrite")
    assert '"git-info-refs"' in route
    assert '"git-upload-pack"' in route
    assert '"release-blob"' in route
    assert "return await _https_mirror_proxy" in route
    assert "action in REPO_HOST_READ_ACTIONS" in route

    git_block = route.split("git_info = GIT_INFO_RE.match", 1)[1].split(
        'if url.path == "/"', 1)[0]
    assert "_https_mirror_proxy" in git_block
    assert "FORKMESH_HOST" not in git_block

    release_block = route.split(
        "release_blob_match = RELEASE_BLOB_RE.match", 1)[1].split(
            "host_match = REPO_HOST_RE.match", 1)[0]
    assert "_https_mirror_proxy" in release_block
    assert "FORKMESH_HOST" not in release_block

    browse_fast_path = route.split(
        "host_match = REPO_HOST_RE.match", 1)[1].split(
            "# Registering as a host", 1)[0]
    assert "_https_mirror_proxy" in browse_fast_path
    assert "_https_mirror_private_proxy" not in browse_fast_path
    assert "_private_replica_not_found" in browse_fast_path
    assert "FORKMESH_HOST" not in browse_fast_path

    proxy = _function_source("_https_mirror_proxy")
    assert "forkmesh-masked-proxy-v1" not in proxy  # canonical lives in policy
    assert "https_routing.request_message" in proxy
    assert "ed25519_sign" in proxy
    assert "JsResponse.new" in proxy
    assert "upstream.body" in proxy
    assert "FORKMESH_HOST" not in proxy
    assert "WebSocket" not in proxy
    # Comments may explain why a bounded header is not authorization input;
    # inspect executable syntax so those explanations do not fail the guard.
    executable_proxy = ast.unparse(ast.parse(proxy)).lower()
    assert "authorization" not in executable_proxy
    assert "cookie" not in executable_proxy
    assert '"redirect": "manual"' in proxy
    assert "status in HTTPS_MIRROR_RETRY_STATUSES" in proxy
    assert "_https_mirror_record_request_rejection" in proxy
    assert "if status in (401, 403, 500, 502, 503, 504):" not in proxy


def test_authorized_private_reads_stream_only_opaque_ciphertext_over_https():
    route = _method_source("Default", "_route")
    registration = _function_source("https_mirror_private_route_handler")
    access = _function_source("_https_mirror_private_access_handler")
    lookup = _function_source("_https_mirror_private_access_record")
    authorization = _function_source(
        "_https_mirror_private_request_authorized")
    candidates = _function_source("_https_mirror_private_candidates")
    proxy = _function_source("_https_mirror_private_proxy")
    urls = URLS.read_text(encoding="utf-8")
    assert "PRIVATE_REPLICA_ACCESS_RE" in route
    assert route.index("PRIVATE_REPLICA_ACCESS_RE") < route.index(
        "org_alias_rewrite")
    assert "REPO_PRIVATE_REPLICA_RE" not in route
    assert r"^/api/private-replicas/([0-9a-f]{64})$" in urls
    assert "/api/repo/([^/]+)/([^/]+)/private-replica" not in urls
    assert "url.query" in access
    assert 'method_name(request) not in ("GET", "HEAD")' in access
    assert "_https_mirror_private_access_record" in access
    assert "_https_mirror_private_request_authorized" in access
    assert "_private_replica_not_found" in access
    assert "_https_mirror_private_proxy" in access
    assert "parse_qs" not in authorization
    assert "urlparse" not in authorization
    assert "_basic_auth_view_ok" in authorization
    assert "opaque_replica_id=? AND active=1" in lookup
    assert "SELECT DISTINCT repo_bi,key_epoch" in lookup
    assert "len(repo_bis) != 1" in lookup
    assert "decrypt_row" in lookup
    assert "blind_index(env, owner + \"/\" + repo)" in lookup
    assert "private_mirror_routes" in registration
    assert '"accessPath"' in registration
    assert '"/api/private-replicas/" + binding["opaqueId"]' in registration
    assert "WHERE opaque_replica_id=? AND repo_bi<>?" in registration
    assert "https_routing.private_route_message" in _function_source(
        "_https_mirror_private_route_payload")
    assert "_owner_pubkey" in registration
    assert "ed25519_verify" in registration
    assert "privateRoutesListed" in registration
    assert "JOIN mirror_https_endpoints" in candidates
    assert "r.repo_bi=?" in candidates
    assert "r.key_epoch=?" in candidates
    assert "https_routing.masked_private_replica_url" in proxy
    assert "application/vnd.forkmesh.private-replica+json" in proxy
    assert "hmac.compare_digest(etag, expected_etag)" in proxy
    assert "upstream.body" in proxy
    assert "authorization" not in proxy.lower()
    assert "cookie" not in proxy.lower()
    assert '"redirect": "manual"' in proxy
    assert "WebSocket" not in proxy


def test_private_catalog_discloses_opaque_access_only_after_viewer_acl():
    catalog = _function_source("catalog_handler")
    access_id = _function_source(
        "_https_mirror_private_catalog_access_id")
    assert catalog.index("if not authed_viewer:") < catalog.index(
        'rec["privateAccessId"] = access_id')
    assert "repo_shares" in catalog
    assert "_https_mirror_private_catalog_access_id" in catalog
    assert '"privateAccessPath"' in catalog
    assert "no-store, max-age=0, must-revalidate" in catalog
    assert "WHERE repo_bi=? AND active=1 AND key_epoch=?" in access_id
    assert "ORDER BY updated_at DESC LIMIT 1" in access_id


def test_private_access_request_denials_are_uniform_and_queries_never_lookup():
    namespace = {}
    exec(_function_source("_https_mirror_private_access_handler"), namespace)
    handler = namespace["_https_mirror_private_access_handler"]
    denial = {"status": 404, "body": {"error": "not_found"}}
    proxied = {"status": 200}
    calls = []
    record = {
        "repoBi": "blind",
        "owner": "alice",
        "repo": "secret",
        "keyEpoch": 2,
    }

    namespace["method_name"] = lambda request: request.method
    namespace["_private_replica_not_found"] = lambda: denial

    async def lookup(env, opaque_id):
        calls.append(("lookup", opaque_id))
        return env.get("record")

    async def authorize(env, request, private_record):
        calls.append(("authorize", private_record["repoBi"]))
        return env.get("authorized", False)

    async def proxy(env, request, private_record):
        calls.append(("proxy", private_record["repoBi"]))
        return proxied

    namespace["_https_mirror_private_access_record"] = lookup
    namespace["_https_mirror_private_request_authorized"] = authorize
    namespace["_https_mirror_private_proxy"] = proxy
    request = SimpleNamespace(method="GET")
    clean_url = SimpleNamespace(query="")
    query_url = SimpleNamespace(query="viewer=alice")

    assert asyncio.run(handler(
        {}, request, clean_url, "a" * 64)) == denial
    unknown_calls = list(calls)
    calls.clear()
    assert asyncio.run(handler(
        {"record": record}, request, clean_url, "a" * 64)) == denial
    unauthorized_calls = list(calls)
    calls.clear()
    assert asyncio.run(handler(
        {"record": record, "authorized": True},
        request, clean_url, "a" * 64)) == proxied
    authorized_calls = list(calls)
    calls.clear()
    assert asyncio.run(handler(
        {"record": record, "authorized": True},
        request, query_url, "a" * 64)) == denial
    query_calls = list(calls)

    assert unknown_calls == [("lookup", "a" * 64)]
    assert unauthorized_calls == [
        ("lookup", "a" * 64), ("authorize", "blind")]
    assert authorized_calls == [
        ("lookup", "a" * 64), ("authorize", "blind"), ("proxy", "blind")]
    assert query_calls == []


def test_receive_pack_fails_closed_without_host_socket_or_request_body():
    # Push is now the signed masked-proxy path (mobile-node-contract.md §7).
    # The route method itself still never touches a socket fallback or the
    # request body; the proxy reads the body only after the signed device
    # headers are present, so unsigned probes stay body-free.
    push = _method_source("Default", "_git_push")
    assert "_https_mirror_push_proxy" in push
    assert "FORKMESH_HOST" not in push
    assert "durable_object_request" not in push
    assert "request.bytes" not in push
    assert "request.text" not in push
    proxy = _function_source("_https_mirror_push_proxy")
    # Unauthorized pushes are refused through one helper that also names the
    # Basic scheme a stock git needs (issue #676), still before any body read.
    assert "push_unavailable" in proxy
    assert proxy.index("x-forkmesh-pubkey") < proxy.index("request.bytes")
    assert proxy.index("_git_push_basic_challenge()") < proxy.index(
        "request.bytes")
    challenge = _function_source("_git_push_basic_challenge")
    assert "push_unauthorized" in challenge
    assert 'Basic realm="forkmesh"' in challenge
    assert "FORKMESH_HOST" not in proxy
    assert "durable_object_request" not in proxy

    # There is no desktop repository host or compatibility socket anymore; the
    # managed Go node is the sole serving process.
    assert not QT_REPO_HOST.exists()
    cmake = (ROOT / "desktop" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "RepoHost.cpp" not in cmake


def test_installer_candidates_are_fresh_https_proofs_not_host_sockets():
    install = _function_source("install_source")
    assert "mirror_https_endpoints" in install
    assert "forkmesh_active=1" in install
    assert "forkmesh_verified_at>=?" in install
    assert "https_routing.ENDPOINT_STALE_MS" in install
    assert "FORKMESH_HOST" not in install
    assert "host_object" not in install
    assert "WebSocket" not in install


def test_selection_is_public_group_scoped_fresh_integrity_and_abuse_gated():
    context = _function_source("_https_mirror_public_context")
    hosted_route = _function_source("_hosted_repository_import_route")
    candidates = _function_source("_https_mirror_candidates")
    proof = _function_source("_https_mirror_repository_proof")
    edge = EDGE.read_text(encoding="utf-8")
    assert "_decrypted_public_catalog" in context
    assert "_hosted_repository_import_route" in context
    assert "status='actively_mirrored'" in hosted_route
    assert "is_private=0" in hosted_route
    # Any publishing node may host an import; the resolved pair still has to
    # appear in the public catalog the caller already filtered.
    assert 'mirror_owner in ("mirror2", "mirror3")' not in hosted_route
    assert "valid_node_name(mirror_owner)" in hosted_route
    assert "safe_segment(mirror_name)" in hosted_route
    assert "visibility" in context
    assert "repo_mirror_same_group" in context
    assert "clone_state_pins" in context
    assert '"currentNodes": current_nodes' in context
    assert '"currentPins": set(current_pins)' in context
    assert "FROM org_repos" in context
    assert 'target.get("stateHash", "")' in context
    assert '"remote-clone"' in context
    assert "context[\"nodes\"]" in candidates
    assert "preferred_region" in candidates
    assert "select_endpoints" in candidates
    assert 'context.get("currentNodes", set())' in candidates
    assert "current_records" in candidates
    assert "historical_records" in candidates
    assert candidates.index("current_records") < candidates.index("historical_records")
    assert "ENDPOINT_STALE_MS" in edge
    assert "abuseBlocked" in edge
    assert "integrity" in edge
    assert "MAX_FAILOVER_ATTEMPTS" in edge
    assert "repository_health_challenge" in proof
    assert "refs_digest in context[\"pins\"]" in proof
    assert 'endpoint["refsSha256"] = refs_digest' in proof
    assert 'memo.get("refsSha256", "")' in proof
    assert "operations_digest == claimed_operations_digest" in proof
    assert "ed25519_verify" in proof
    assert "https_routing.repository_proof_cache_get" in proof
    assert "https_routing.repository_proof_cache_put" in proof
    assert "async def repository_proof_cache_get" in edge
    assert "async def repository_proof_cache_put" in edge


def test_failed_repository_proofs_are_memoized_per_isolate_only():
    # A lagging or unreachable endpoint used to be re-probed (up to the full
    # control-fetch timeout) by every request that rotated onto it, keeping
    # the ingress Durable Object resident and burning the shared free-tier
    # duration allowance. The failure verdict is held briefly per isolate;
    # the edge cache still only ever stores valid proofs, so one isolate's
    # transient failure can never evict a good routing answer globally.
    proof = _function_source("_https_mirror_repository_proof")
    entry = ENTRY.read_text(encoding="utf-8")
    assert 'memo.get("failed")' in proof
    assert "HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS" in proof
    assert '"failed": True' in proof
    # The failure memo is written on the invalid path and the edge cache put
    # remains reachable only after a valid proof.
    failure_write = proof.index('"failed": True')
    edge_put = proof.index("repository_proof_cache_put")
    assert failure_write < edge_put
    assert "HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS = 20 * 1000" in entry
    # The failure hold must stay well under the valid-proof TTL so a mirror
    # that catches up is back in rotation quickly.
    failure_ttl = int(re.search(
        r"HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS = (\d+) \* 1000",
        entry).group(1))
    valid_ttl = int(re.search(
        r"HTTPS_MIRROR_REPO_PROOF_TTL_MS = (\d+) \* 1000", entry).group(1))
    assert failure_ttl <= valid_ttl


def _signed_repository_proof_namespace(operations, *, pin, node="mirror40"):
    """Namespace whose endpoint answers one authentic signed proof."""
    now = 1_787_270_534_000
    fetches = []
    digest = routing.operations_sha256(sorted(operations))

    async def fetch_text(target, _max_bytes, _timeout):
        fetches.append(target)
        query = parse_qs(target.split("?", 1)[1])
        message = routing.repository_health_challenge(
            node, query["nonce"][0], int(query["issuedAt"][0]),
            query["owner"][0], query["repo"][0], True, "ok", pin, digest)
        return 200, json.dumps({
            "ok": True,
            "node": node,
            "publicKey": "public-key",
            "transport": "direct-https",
            "integrity": "ok",
            "repositoryProof": {
                "owner": query["owner"][0],
                "repository": query["repo"][0],
                "available": True,
                "integrity": "ok",
                "refsSha256": pin,
                "operations": sorted(operations),
                "operationsSha256": digest,
            },
            "challenge": {
                "messageType": "forkmesh-https-health-repository-v1",
                "nonce": query["nonce"][0],
                "issuedAt": int(query["issuedAt"][0]),
                "algorithm": "Ed25519",
                "encoding": "base64url-no-padding",
                "messageSha256": hashlib.sha256(
                    message.encode("utf-8")).hexdigest(),
                "signature": "challenge-signature",
            },
        })

    class _Cache:
        async def match(self, _key):
            return None

        async def put(self, _key, _response):
            return None

    async def verify(*_args):
        return True

    namespace = {
        "Date": SimpleNamespace(now=lambda: now),
        "json": json,
        "hashlib": hashlib,
        "quote": quote,
        "_b64url_encode": lambda raw: "nonce-value-0123456789",
        "_random_bytes": lambda size: bytes(size),
        "https_routing": routing,
        "js_caches": SimpleNamespace(default=_Cache()),
        "JsResponse": SimpleNamespace(new=lambda *_args: object()),
        "to_js": lambda value: value,
        "ed25519_verify": verify,
        "_https_mirror_fetch_text": fetch_text,
        "_HTTPS_MIRROR_REPO_PROOF_MEMO": {},
        "HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS": 20_000,
        "HTTPS_MIRROR_REPO_PROOF_TTL_MS": 60_000,
        "HTTPS_MIRROR_REPO_PROOF_MEMO_MAX": 256,
        "HTTPS_MIRROR_REPO_PROOF_EDGE_PREFIX": "proof:",
        "HTTPS_MIRROR_MANIFEST_MAX_BYTES": 16_384,
        "HTTPS_MIRROR_CONTROL_FETCH_TIMEOUT_SECONDS": 5,
    }
    exec(_function_source("_https_mirror_repository_proof"), namespace)
    return namespace, fetches


def test_an_unadvertised_operation_is_not_an_endpoint_failure():
    # A read-only mirror answers an owner's merge probe with a perfectly
    # authentic, healthy, pin-matching proof that simply does not list
    # merge-pull. Holding that as a node failure took the mirror out of
    # ordinary read routing for the whole failure window, so one attempted web
    # merge degraded the repository for every visitor behind it.
    pin = "a" * 64
    reads = ["blob", "git-info-refs", "git-upload-pack", "raw", "tree"]
    namespace, fetches = _signed_repository_proof_namespace(reads, pin=pin)
    proof = namespace["_https_mirror_repository_proof"]
    endpoint = {
        "node": "mirror40",
        "baseUrl": "https://mirror40.example.test",
        "publicKey": "public-key",
    }
    context = {"owner": "forkmesh", "repo": "forkmesh", "pins": {pin}}

    details = {}
    assert not asyncio.run(proof(
        {}, dict(endpoint), context, "merge-pull", details=details))
    assert details["reason"] == "git_operation_unavailable"
    assert details["integrity"] == "ok"
    assert details["signed"] is True
    assert len(fetches) == 1

    memo = namespace["_HTTPS_MIRROR_REPO_PROOF_MEMO"]
    assert [entry.get("failed") for entry in memo.values()] == [None]

    # The same endpoint still serves everything it does advertise, answered
    # from that one verified proof rather than a second probe.
    served = dict(endpoint)
    assert asyncio.run(proof({}, served, context, "blob"))
    assert served["refsSha256"] == pin
    assert len(fetches) == 1


def test_an_advertised_merge_capability_admits_the_endpoint():
    pin = "b" * 64
    namespace, fetches = _signed_repository_proof_namespace(
        ["blob", "merge-pull", "raw", "tree", "update-pull-branch"], pin=pin)
    proof = namespace["_https_mirror_repository_proof"]
    endpoint = {
        "node": "mirror40",
        "baseUrl": "https://mirror40.example.test",
        "publicKey": "public-key",
    }
    context = {"owner": "forkmesh", "repo": "forkmesh", "pins": {pin}}
    details = {}
    assert asyncio.run(proof(
        {}, dict(endpoint), context, "merge-pull", details=details))
    assert details["reason"] == "verified"
    assert len(fetches) == 1


def test_installer_source_fallback_can_retry_a_failed_proof_memo():
    """The fallback is a real second attempt, not a memoized first failure."""
    now = 1_787_270_534_000
    pin = "a" * 64
    endpoint = {
        "node": "mirror26",
        "baseUrl": "https://mirror26.invalid",
    }
    context = {
        "owner": "forkmesh",
        "repo": "forkmesh",
        "pins": {pin},
    }
    memo_key = (
        "mirror26", "https://mirror26.invalid", "forkmesh", "forkmesh", pin)
    cache_reads = []

    async def cache_get(*_args):
        cache_reads.append(True)
        return {
            "checkedAt": now,
            "operations": frozenset({"blobs"}),
            "refsSha256": pin,
        }

    namespace = {
        "Date": SimpleNamespace(now=lambda: now),
        "_HTTPS_MIRROR_REPO_PROOF_MEMO": {
            memo_key: {
                "checkedAt": now,
                "operations": frozenset(),
                "refsSha256": "",
                "failed": True,
            },
        },
        "HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS": 20_000,
        "HTTPS_MIRROR_REPO_PROOF_TTL_MS": 60_000,
        "HTTPS_MIRROR_REPO_PROOF_MEMO_MAX": 256,
        "HTTPS_MIRROR_REPO_PROOF_EDGE_PREFIX": "proof:",
        "https_routing": SimpleNamespace(
            normalize_base_url=lambda value: str(value or "").rstrip("/"),
            repository_proof_cache_key=lambda *_args: "proof:key",
            repository_proof_cache_get=cache_get,
        ),
        "js_caches": SimpleNamespace(default=object()),
    }
    exec(_function_source("_https_mirror_repository_proof"), namespace)
    proof = namespace["_https_mirror_repository_proof"]

    assert not asyncio.run(proof({}, dict(endpoint), context, "blobs"))
    assert cache_reads == []

    retried_endpoint = dict(endpoint)
    assert asyncio.run(proof(
        {}, retried_endpoint, context, "blobs", retry_failed=True))
    assert cache_reads == [True]
    assert retried_endpoint["refsSha256"] == pin


def test_signed_refs_lag_serves_release_blobs_and_two_agreeing_browse_reads():
    # Issue #692: after main moved, every mirror served the new refs state
    # while the pin set still named the old one, so each candidate failed
    # refs_mismatch and the Windows installer answered mirror_unavailable.
    servable = routing.mismatch_is_servable
    ahead = "b" * 64

    def lag(**overrides):
        details = {
            "reason": "refs_mismatch",
            "signed": True,
            "refsSha256": ahead,
            "operations": ["blob", "blobs", "tree", "release-blob",
                           "git-info-refs", "git-upload-pack"],
        }
        details.update(overrides)
        return details

    # Content-addressed release bytes do not depend on the refs state.
    assert servable("release-blob", "mirror35", lag(), {})
    # A browse read needs a second distinct node with the same signed digest.
    agreeing = {}
    assert not servable("blob", "mirror35", lag(), agreeing)
    assert not servable("blob", "mirror35", lag(), agreeing)
    assert servable("blob", "mirror36", lag(), agreeing)
    other = {}
    assert not servable("tree", "mirror35", lag(), other)
    assert not servable("tree", "mirror36", lag(refsSha256="c" * 64), other)
    # A clone takes the same two-node quorum (adhoc #243); one node alone, or
    # two nodes on different digests, still cannot advertise.
    for operation in ("git-info-refs", "git-upload-pack"):
        seen = {}
        assert not servable(operation, "mirror35", lag(), seen)
        assert not servable(
            operation, "mirror37", lag(refsSha256="c" * 64), seen)
        assert servable(operation, "mirror36", lag(), seen)
    # A push never rides a lagging proof.
    seen = {}
    pushable = lag(operations=["git-receive-pack"])
    assert not servable("git-receive-pack", "mirror35", pushable, seen)
    assert not servable("git-receive-pack", "mirror36", pushable, seen)
    # Nothing but a signed refs lag qualifies.
    assert not servable("release-blob", "mirror35", lag(signed=False), {})
    for reason in ("repository_signature_invalid", "mirror_unhealthy",
                   "repository_unavailable", "repository_integrity_failed",
                   "repository_proof_failed"):
        assert not servable("release-blob", "mirror35", lag(reason=reason), {})
    assert not servable("release-blob", "mirror35", lag(refsSha256="nope"), {})
    assert not servable("release-blob", "mirror35", lag(operations=["blob"]), {})
    assert not servable("release-blob", "mirror35", None, {})


def test_held_refs_lag_verdict_is_replayed_to_the_caller():
    now = 1_787_270_534_000
    pin = "a" * 64
    ahead = "b" * 64
    endpoint = {"node": "mirror36", "baseUrl": "https://mirror36.invalid"}
    context = {"owner": "forkmesh", "repo": "forkmesh", "pins": {pin}}
    memo_key = (
        "mirror36", "https://mirror36.invalid", "forkmesh", "forkmesh", pin)
    namespace = {
        "Date": SimpleNamespace(now=lambda: now),
        "_HTTPS_MIRROR_REPO_PROOF_MEMO": {
            memo_key: {
                "checkedAt": now,
                "operations": frozenset({"release-blob", "blob"}),
                "refsSha256": ahead,
                "failed": True,
                "reason": "refs_mismatch",
                "signed": True,
                "integrity": "failed",
            },
        },
        "HTTPS_MIRROR_REPO_PROOF_FAILURE_TTL_MS": 20_000,
        "HTTPS_MIRROR_REPO_PROOF_TTL_MS": 60_000,
        "HTTPS_MIRROR_REPO_PROOF_MEMO_MAX": 256,
        "HTTPS_MIRROR_REPO_PROOF_EDGE_PREFIX": "proof:",
        "https_routing": SimpleNamespace(
            normalize_base_url=lambda value: str(value or "").rstrip("/"),
        ),
        "js_caches": SimpleNamespace(default=object()),
    }
    exec(_function_source("_https_mirror_repository_proof"), namespace)
    proof = namespace["_https_mirror_repository_proof"]
    details = {}
    assert not asyncio.run(proof(
        {}, dict(endpoint), context, "release-blob", details=details))
    assert details["reason"] == "refs_mismatch"
    assert details["signed"] is True
    assert details["refsSha256"] == ahead
    assert details["operations"] == ["blob", "release-blob"]
    # The proxy consults the verdict before skipping an endpoint, and the
    # held failure still never reaches the shared edge cache.
    proxy = _function_source("_https_mirror_proxy")
    assert "lagging_refs_quorum = {}" in proxy
    assert "details=proof_details" in proxy
    assert "https_routing.mismatch_is_servable(" in proxy
    assert "def _https_mirror_mismatch_is_servable" not in ENTRY.read_text(encoding="utf-8")
    source = _function_source("_https_mirror_repository_proof")
    assert source.index('"failed": True') < source.index(
        "repository_proof_cache_put")


def test_pending_endpoint_registrations_are_held_without_reverification():
    # A node whose refs lag the accepted pin retries its lease every few
    # seconds. Each retry used to re-run DNS, manifest, and the signed health
    # probe inside the ingress object. An unchanged pending identity is now
    # answered from a short per-isolate hold, before the DNS/manifest fetches;
    # acceptance is delayed by at most the hold window.
    handler = _function_source("https_mirror_endpoint_handler")
    assert "_HTTPS_MIRROR_REGISTRATION_PENDING_MEMO" in handler
    assert "HTTPS_MIRROR_REGISTRATION_PENDING_HOLD_MS" in handler
    hold_check = handler.index("HTTPS_MIRROR_REGISTRATION_PENDING_HOLD_MS")
    dns_check = handler.index("_https_mirror_cloudflare_dns_ok")
    manifest_check = handler.index("_https_mirror_manifest_ok")
    assert hold_check < dns_check < manifest_check
    # The hold is scoped to the exact registered identity and is dropped the
    # moment the endpoint verifies active, so it can never mask a rotation or
    # delay a healthy node.
    assert 'held.get("identity") == identity' in handler
    assert handler.count(
        "_HTTPS_MIRROR_REGISTRATION_PENDING_MEMO.pop") == 2
    assert "if health_active:" in handler


def test_request_transients_do_not_quarantine_shared_endpoint_health():
    # One page load may try every eligible mirror. A request-local timeout or
    # 502/503 must fail over without zeroing the signed health lease for every
    # other visitor; only the recurring health verifier owns that shared state.
    proxy = _function_source("_https_mirror_proxy")
    assert "checked_at=0" not in proxy
    assert "_https_mirror_mark_transient_failure" not in proxy
    assert "_https_mirror_record_request_rejection(" in proxy


def test_public_proxy_preserves_verified_org_alias_for_gateway_bytes():
    proxy = _function_source("_https_mirror_proxy")
    proof = _function_source("_https_mirror_repository_proof")
    assert 'context["routeOwner"] = route_owner' in proxy
    assert 'route_owner != owner' in proxy
    assert 'env, route_owner, route_repo) == owner' in proxy
    assert '== context["owner"]' not in proxy
    assert 'context.get("routeOwner") or context["owner"]' in proxy
    assert 'route_owner = context.get("routeOwner") or context["owner"]' in proof
    assert '"&owner=" + quote(route_owner)' in proof


def test_direct_repository_check_requires_signed_refs_and_exact_git_url():
    requests = []
    ticks = iter((1_000, 1_025, 2_000, 2_020))

    class Headers(dict):
        def get(self, key, default=None):
            return super().get(str(key).lower(), default)

    async def proof(
            _env, endpoint, context, operation, *, origin, force_refresh,
            details):
        assert endpoint["node"] == "mirror9"
        assert context["pins"] == {"a" * 64}
        assert operation == "git-info-refs"
        assert origin == "https://git.mirror9.example.net"
        assert force_refresh is True
        details.update({
            "integrity": "ok",
            "refsSha256": "a" * 64,
            "status": 200,
            "reason": "verified",
        })
        return True

    async def fetch(url, options, timeout):
        requests.append((url, options, timeout))
        return SimpleNamespace(
            status=200,
            headers=Headers({
                "content-type": (
                    "application/x-git-upload-pack-advertisement")
            }),
        )

    namespace = {
        "Date": SimpleNamespace(now=lambda: next(ticks)),
        "https_routing": routing,
        "_https_mirror_repository_proof": proof,
        "js_fetch_with_timeout": fetch,
        "HTTPS_MIRROR_CONTROL_FETCH_TIMEOUT_SECONDS": 5,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(_function_source("_https_mirror_check_direct_repository"), namespace)
    check = namespace["_https_mirror_check_direct_repository"]
    endpoint = {
        "node": "mirror9",
        "mirrorUrl": "https://git.mirror9.example.net",
        "issuedAt": 900,
        "abuseBlocked": False,
    }
    context = {
        "owner": "source-node",
        "repo": "project",
        "pins": {"a" * 64},
    }

    result = asyncio.run(check(object(), endpoint, context))

    assert result == {
        "mirror_url": "https://git.mirror9.example.net",
        "registration_issued_at": 900,
        "reachable": 1,
        "integrity": "ok",
        "refs_sha256": "a" * 64,
        "checked_at": 1_025,
        "latency_ms": 25,
        "status_code": 200,
        "reason": "verified",
    }
    assert requests == [(
        "https://git.mirror9.example.net/source-node/project.git/"
        "info/refs?service=git-upload-pack",
        {
            "method": "HEAD",
            "headers": {
                "Accept": "application/x-git-upload-pack-advertisement"},
            "redirect": "manual",
        },
        5,
    )]

    async def mismatched(
            _env, _endpoint, _context, _operation, **kwargs):
        kwargs["details"].update({
            "integrity": "failed",
            "refsSha256": "b" * 64,
            "status": 200,
            "reason": "refs_mismatch",
        })
        return False

    namespace["_https_mirror_repository_proof"] = mismatched
    requests.clear()
    rejected = asyncio.run(check(object(), endpoint, context))
    assert rejected["reachable"] == 0
    assert rejected["integrity"] == "failed"
    assert rejected["refs_sha256"] == "b" * 64
    assert rejected["reason"] == "refs_mismatch"
    assert requests == []


def test_stored_direct_verdict_does_not_become_repository_identity():
    namespace = {
        "https_routing": routing,
        "hmac": __import__("hmac"),
        "re": re,
        "MAX_NODE_NAME": 80,
        "clean_string": lambda value, limit: str(value or "")[:limit],
    }
    exec(_function_source("_mirror_direct_integrity_fields"), namespace)
    project = namespace["_mirror_direct_integrity_fields"]
    record = {
        "mirror_url": "https://old.example.net",
        "registration_issued_at": 900,
        "reachable": 1,
        "integrity": "ok",
        "refs_sha256": "a" * 64,
        "checked_at": 1_000,
        "latency_ms": 12,
        "status_code": 200,
        "reason": "verified",
    }

    verified = project(
        "mirror9", "https://old.example.net", "source-node", "project",
        record, registration_issued_at=900)
    replaced = project(
        "mirror9", "https://new.example.net", "source-node", "project",
        record, registration_issued_at=900)
    rotated = project(
        "mirror9", "https://old.example.net", "source-node", "project",
        record, registration_issued_at=901)

    assert verified["directVerified"] is True
    assert verified["directRefsSha256"] == "a" * 64
    assert replaced["mirrorUrl"] == (
        "https://new.example.net/source-node/project.git")
    assert replaced["directVerified"] is False
    assert replaced["directCheckedAt"] == 0
    assert replaced["directReason"] == "unchecked"
    assert rotated["directVerified"] is False
    assert rotated["directReason"] == "unchecked"


def test_website_direct_check_is_persisted_and_rate_bounded():
    handler = _function_source("repo_mirror_reachability_handler")
    direct_check = _function_source("_https_mirror_check_direct_repository")
    proof = _function_source("_https_mirror_repository_proof")
    mirrors = _function_source("repo_mirrors_handler")

    assert 'request_method not in ("GET", "POST")' in handler
    assert "INSERT INTO mirror_repository_integrity" in handler
    assert "ON CONFLICT(repo_bi,node_bi) DO UPDATE SET" in handler
    assert "HTTPS_MIRROR_DIRECT_CHECK_COOLDOWN_MS" in handler
    assert "0 <= existing_age" in handler
    assert "edge_cache_delete" in handler
    assert "force_refresh=True" in direct_check
    assert "if not force_refresh:" in proof
    assert "FROM mirror_repository_integrity" in mirrors
    assert 'direct_fields.pop("mirrorUrl", None)' in mirrors


def test_stateless_rotation_preserves_two_request_git_clone_stickiness():
    candidates = _function_source("_https_mirror_candidates")
    advance = _function_source("_https_mirror_route_advance")
    assert 'cursor = int.from_bytes(_random_bytes(4), "big")' in candidates
    assert "SELECT cursor FROM edge_route_cursor" not in candidates
    assert "edge_route_cursor" not in advance
    assert 'if operation == "git-info-refs"' in advance
    assert "clone_sticky" in advance


def test_d1_schema_is_metadata_only_and_contains_no_repository_bytes():
    migration = MIGRATION.read_text(encoding="utf-8")
    private_migration = PRIVATE_MIGRATION.read_text(encoding="utf-8")
    private_access_migration = PRIVATE_ACCESS_MIGRATION.read_text(
        encoding="utf-8")
    schema = SCHEMA.read_text(encoding="utf-8")
    for text in (migration, schema):
        assert "mirror_https_endpoints" in text
        assert "edge_route_cursor" in text
        assert "forkmesh_verified_at" in text
        assert "forkmesh_refs_sha256" in text
        assert "forkmesh_operations_sha256" in text
    assert "repository bytes" in migration.lower()
    assert "private key" in migration.lower()
    assert " BLOB" not in migration
    for text in (private_migration, schema):
        assert "private_mirror_routes" in text
        assert "opaque_replica_id" in text
        assert "replica_sha256" in text
        assert "key_epoch" in text
    assert "repository identities" in private_migration.lower()
    assert "private keys" in private_migration.lower()
    assert " BLOB" not in private_migration
    assert "owner TEXT" not in private_migration
    assert "repository TEXT" not in private_migration
    for text in (private_access_migration, schema):
        assert "idx_private_mirror_route_access" in text
        assert (
            "opaque_replica_id, active, key_epoch, repo_bi" in text)


def test_https_and_private_route_migrations_apply_together():
    database = sqlite3.connect(":memory:")
    try:
        database.executescript(MIGRATION.read_text(encoding="utf-8"))
        database.executescript(PRIVATE_MIGRATION.read_text(encoding="utf-8"))
        database.executescript(
            PRIVATE_ACCESS_MIGRATION.read_text(encoding="utf-8"))
        columns = {
            row[1]
            for row in database.execute(
                "PRAGMA table_info(private_mirror_routes)")
        }
        assert columns == {
            "binding_bi", "repo_bi", "node_bi", "opaque_replica_id",
            "replica_sha256", "key_epoch", "owner_sig", "issued_at",
            "active", "updated_at",
        }
        indexes = {
            row[1]
            for row in database.execute(
                "PRAGMA index_list(private_mirror_routes)")
        }
        assert "idx_private_mirror_route_access" in indexes
    finally:
        database.close()


class JsNull:
    """Stand-in for pyodide.ffi.jsnull, which Workers' Headers.get answers.

    It is deliberately not None and stringifies to the literal "jsnull" — the
    exact shape that leaked absent upstream headers onto the wire.
    """

    def __str__(self):
        return "jsnull"


class _WorkersHeaders:
    """Headers whose get() answers JS null for anything the endpoint omitted."""

    def __init__(self, present):
        self._present = dict(present)

    def get(self, name):
        return self._present.get(name, JsNull())


def _proxy_namespace(responses, cached_response=None, *, direct=False):
    """Run _https_mirror_proxy over a scripted per-node upstream reply list."""
    attempts = []

    class Upstream:
        def __init__(self, status):
            self.status = status
            self.headers = _WorkersHeaders({
                "content-type": "application/json",
                "content-disposition": 'attachment; filename="digest"',
            })
            self.body = "body:%d" % status

    async def fetch_with_timeout(target, init, timeout):
        attempts.append(target)
        reply = responses[len(attempts) - 1]
        if reply is None:
            raise RuntimeError("transport failure")
        return Upstream(reply)

    async def _true(*_args, **_kwargs):
        return True

    async def _none(*_args, **_kwargs):
        return None

    async def _false(*_args, **_kwargs):
        return False

    async def cache_get(*_args, **_kwargs):
        return cached_response

    async def candidates(_env, _context, _country, sticky="", **_kwargs):
        return [
            {"node": "mirror9", "baseUrl": "https://a.invalid",
             "refsSha256": "a" * 64,
             **({"mirrorUrl": "https://git-a.invalid"} if direct else {})},
            {"node": "mirror14", "baseUrl": "https://b.invalid",
             "refsSha256": "b" * 64},
        ]

    namespace = {
        "clean_string": lambda value, _limit: str(value or ""),
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 64,
        "safe_segment": lambda value: str(value or ""),
        "_https_mirror_public_context_copy": lambda value: value,
        "_https_mirror_public_context": _proxy_context,
        "json_response": _proxy_json_response,
        "urlparse": _urlparse,
        "parse_qs": parse_qs,
        "GIT_INFO_RE": re.compile(r"^\0$"),
        "GIT_PACK_RE": re.compile(r"^\0$"),
        "RELEASE_BLOB_RE": re.compile(r"^\0$"),
        "REPO_HOST_RE": re.compile(r"^\0$"),
        "_org_repo_node": _none,
        "blind_index": _none,
        "method_name": lambda _request: "GET",
        "_https_mirror_request_query": lambda *_a, **_k: {},
        "request_bypasses_repository_metadata_cache": lambda _request: True,
        "release_download_content_disposition": (
            release_policy.release_download_content_disposition),
        "repository_metadata_cache_key": (
            lambda *_a, **_k:
            "cache-key" if cached_response is not None else ""),
        "repository_metadata_cache_get": cache_get,
        "repository_metadata_cache_put": _none,
        "HTTPS_MIRROR_BODY_MAX_BYTES": 1 << 20,
        "_https_mirror_router_public_key": lambda _env: "router-key",
        "_https_mirror_router_seed": lambda _env: "router-seed",
        "_https_mirror_clone_pin": _none,
        "_https_mirror_request_candidates": candidates,
        "world_request_country": lambda _request: "",
        "_https_mirror_repository_proof": _true,
        "https_routing": SimpleNamespace(
            normalize_base_url=lambda value: str(value or "").rstrip("/"),
            direct_git_target_url=lambda base, owner, repo, operation, query:
                base + "/" + owner + "/" + repo + ".git/info/refs",
            masked_target_url=lambda base, owner, repo, operation, query:
                base + "/v1/repositories/" + owner + "/" + repo
                + "/" + operation,
            request_message=lambda *_a: "message",
            response_headers=lambda headers: dict(headers),
        ),
        "hashlib": hashlib,
        "_b64url_encode": lambda raw: "request-id",
        "_random_bytes": lambda count: b"\0" * count,
        "Date": SimpleNamespace(now=lambda: 0),
        "ed25519_sign": _proxy_sign,
        "js_fetch_with_timeout": fetch_with_timeout,
        "HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS": 5,
        "HTTPS_MIRROR_RETRY_STATUSES": frozenset(
            {401, 403, 404, 408, 425, 429, 500, 502, 503, 504}),
        "_https_mirror_record_request_rejection": _false,
        "_https_mirror_route_advance": _none,
        "JsResponse": SimpleNamespace(
            new=lambda body, init: SimpleNamespace(body=body, init=init)),
        "to_js": lambda value: value,
        "EXPECTED_DEGRADED_HEADERS": {},
        "Response": lambda body, status=200, headers=None: SimpleNamespace(
            status=status, headers=headers or {}),
    }
    exec(_function_source("js_nullish"), namespace)
    exec(_function_source("_https_mirror_proxy"), namespace)
    return namespace["_https_mirror_proxy"], attempts


async def _proxy_context(_env, owner, repo):
    return {
        "owner": owner, "repo": repo, "repoBi": "repo-bi",
        "currentPins": set(),
    }


async def _proxy_sign(_public_key, _seed, _message):
    return "signature"


def _proxy_json_response(payload, status=200, **_kwargs):
    return SimpleNamespace(payload=payload, status=status)


def _urlparse(value):
    from urllib.parse import urlparse as parse
    return parse(str(value or ""))


def _run_proxy(responses, operation="tree"):
    proxy, attempts = _proxy_namespace(responses)
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/forkmesh/tree",
        headers={},
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", operation))
    return answer, attempts


def test_a_path_every_mirror_agrees_is_absent_answers_not_found_not_outage():
    # The regression (adhoc #1723): a mirror reports a ref/path it does not
    # hold with its own 404, and the proxy counted that as an endpoint failure.
    # With every healthy mirror agreeing, the visitor got 503
    # "mirror_unavailable" and the site printed "Pull requests are unavailable
    # until a reachable mirror host serves the .forkmesh/pulls/ folder" for a
    # repository whose pull ledger is simply empty.
    answer, attempts = _run_proxy([404, 404])
    assert len(attempts) == 2
    assert answer.status == 404
    assert answer.payload == {"error": "not_found"}


def _run_lagging_clone(responses, operation, digests, sticky=""):
    """Drive the proxy while every mirror's signed refs are ahead of the pins."""
    proxy, attempts = _proxy_namespace(responses)
    namespace = proxy.__globals__

    async def lagging_proof(_env, endpoint, _context, _operation, **kwargs):
        details = kwargs.get("details")
        if isinstance(details, dict):
            details.update({
                "reason": "refs_mismatch",
                "signed": True,
                "refsSha256": digests[endpoint["node"]],
                "operations": ["git-info-refs", "git-upload-pack", "tree"],
            })
        return False

    async def clone_pin(*_args, **_kwargs):
        return sticky

    async def request_bytes():
        return b"0000"

    namespace["_https_mirror_repository_proof"] = lagging_proof
    namespace["_https_mirror_clone_pin"] = clone_pin
    namespace["https_routing"].mismatch_is_servable = (
        routing.mismatch_is_servable)
    namespace["method_name"] = lambda _request: (
        "POST" if operation == "git-upload-pack" else "GET")
    namespace["js_fetch_until_response"] = namespace["js_fetch_with_timeout"]
    namespace["HTTPS_MIRROR_GIT_INITIAL_RESPONSE_TIMEOUT_SECONDS"] = 5
    namespace["Uint8Array"] = SimpleNamespace(new=lambda value: value)
    namespace["_to_js"] = lambda value: value
    request = SimpleNamespace(
        url="https://app.forkmesh.com/forkmesh/forkmesh/info/refs",
        headers={},
        bytes=request_bytes,
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", operation))
    return answer, attempts


def test_clone_is_served_while_the_pins_lag_two_agreeing_mirrors():
    # adhoc #243: `git clone` answered 503 mirror_unavailable for the minute or
    # two after every ref move, because both mirrors already signed the new
    # refs digest while the pin set still named the old one.
    ahead = {"mirror9": "b" * 64, "mirror14": "b" * 64}
    answer, attempts = _run_lagging_clone([200], "git-info-refs", ahead)
    assert answer.init["status"] == 200
    assert answer.init["headers"]["X-ForkMesh-Served-By"] == "mirror14"
    assert attempts == [
        "https://b.invalid/v1/repositories/forkmesh/forkmesh/git-info-refs"]

    # Two mirrors that disagree are still not an advertisement.
    split = {"mirror9": "b" * 64, "mirror14": "c" * 64}
    answer, attempts = _run_lagging_clone([200], "git-info-refs", split)
    assert answer.status == 503
    assert attempts == []


def test_lagging_upload_pack_returns_to_its_advertising_mirror():
    ahead = {"mirror9": "b" * 64, "mirror14": "b" * 64}
    # The quorum forms on the second candidate, yet the pack request goes back
    # to the node that served info/refs.
    answer, attempts = _run_lagging_clone(
        [200], "git-upload-pack", ahead, sticky="mirror9")
    assert answer.init["headers"]["X-ForkMesh-Served-By"] == "mirror9"
    assert attempts == [
        "https://a.invalid/v1/repositories/forkmesh/forkmesh/git-upload-pack"]

    # When that node fails in transport the agreeing peer, which holds the
    # identical refs set, answers instead — once, not in a loop.
    answer, attempts = _run_lagging_clone(
        [None, 200], "git-upload-pack", ahead, sticky="mirror9")
    assert answer.init["headers"]["X-ForkMesh-Served-By"] == "mirror14"
    assert [target.split("/")[2] for target in attempts] == [
        "a.invalid", "b.invalid"]

    answer, attempts = _run_lagging_clone(
        [None, None], "git-upload-pack", ahead, sticky="mirror9")
    assert answer.status == 503
    assert len(attempts) == 2


def test_release_blob_response_uses_the_manifest_selected_filename():
    proxy, attempts = _proxy_namespace([200])
    request = SimpleNamespace(
        url=(
            "https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/"
            "blob/sha256/" + "a" * 64
            + "?filename=forkmesh-linux-x86_64.AppDir.tar.gz"
        ),
        headers={},
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "release-blob",
        release_sha="a" * 64,
    ))

    assert len(attempts) == 1
    assert answer.init["headers"]["content-disposition"] == (
        'attachment; filename="forkmesh-linux-x86_64.AppDir.tar.gz"; '
        "filename*=UTF-8''forkmesh-linux-x86_64.AppDir.tar.gz"
    )


def test_release_blob_response_drops_headers_the_gateway_never_sent():
    # Observed live on the release download route (issue #635): the response
    # carried `content-range: jsnull`, `accept-ranges: jsnull`, `etag: jsnull`
    # and `last-modified: jsnull`. Workers' Headers.get answers JS null for an
    # absent header, jsnull is not None, and str(jsnull) is "jsnull" — so an
    # `is not None` guard copied the sentinel through as a header value. A
    # gateway that omits content-disposition would have shipped that same
    # sentinel as the disposition, sending the browser back to the digest in
    # the URL for the filename.
    proxy, _ = _proxy_namespace([200])
    request = SimpleNamespace(
        url=(
            "https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/"
            "blob/sha256/" + "a" * 64 + "?filename=forkmesh-linux-x86_64"
        ),
        headers={},
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "release-blob",
        release_sha="a" * 64,
    ))

    headers = answer.init["headers"]
    assert not [
        name for name, value in headers.items() if str(value) == "jsnull"
    ]
    for absent in ("content-range", "accept-ranges", "etag", "last-modified"):
        assert absent not in headers
    assert headers["content-type"] == "application/json"
    assert headers["content-disposition"] == (
        'attachment; filename="forkmesh-linux-x86_64"; '
        "filename*=UTF-8''forkmesh-linux-x86_64"
    )


def test_release_blob_response_keeps_gateway_name_for_invalid_override():
    proxy, _ = _proxy_namespace([200])
    request = SimpleNamespace(
        url=(
            "https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/"
            "blob/sha256/" + "a" * 64 + "?filename=..%2Fsecret"
        ),
        headers={},
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "release-blob",
        release_sha="a" * 64,
    ))

    assert answer.init["headers"]["content-disposition"] == (
        'attachment; filename="digest"')


def test_a_missing_path_still_fails_over_to_a_fresher_mirror():
    # A mirror lagging inside the pin window can legitimately be missing a path
    # a fresher one already has, so the 404 must keep trying the next endpoint.
    answer, attempts = _run_proxy([404, 200])
    assert len(attempts) == 2
    assert answer.body == "body:200"


def test_unreachable_or_failing_mirrors_still_answer_mirror_unavailable():
    for responses in ([None, None], [503, 500], [404, 500], [500, 404]):
        answer, _ = _run_proxy(responses)
        assert answer.status == 503
        assert answer.payload == {"error": "mirror_unavailable"}


def test_dashboard_live_read_falls_back_to_attested_metadata_during_outage():
    cached = SimpleNamespace(status=200, payload={"ok": True, "cached": True})
    proxy, attempts = _proxy_namespace([None, 503], cached_response=cached)
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/forkmesh/blob",
        headers={"cache-control": "no-cache"},
    )

    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "blob"))

    assert attempts == [
        "https://a.invalid/v1/repositories/forkmesh/forkmesh/blob",
        "https://b.invalid/v1/repositories/forkmesh/forkmesh/blob",
    ]
    assert answer is cached


def test_authoritative_missing_metadata_never_resurrects_a_cached_record():
    cached = SimpleNamespace(status=200, payload={"ok": True, "cached": True})
    proxy, attempts = _proxy_namespace([404, 404], cached_response=cached)
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/forkmesh/blob",
        headers={"cache-control": "no-cache"},
    )

    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "blob"))

    assert len(attempts) == 2
    assert answer.status == 404
    assert answer.payload == {"error": "not_found"}


def test_monitoring_never_uses_the_visitor_metadata_fallback():
    cached = SimpleNamespace(status=200, payload={"ok": True, "cached": True})
    proxy, attempts = _proxy_namespace([None, 503], cached_response=cached)
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/forkmesh/blob",
        headers={"cache-control": "no-cache"},
    )

    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "blob",
        monitoring=True))

    assert len(attempts) == 2
    assert answer.status == 503
    assert answer.payload == {"error": "mirror_unavailable"}


def test_deep_probe_quarantines_a_transport_failing_mirror_for_its_suite():
    proxy, attempts = _proxy_namespace([None, 200, 200])
    request = SimpleNamespace(
        url="https://forkmesh.com/api/repo/forkmesh/forkmesh/tree",
        headers={},
    )
    routing_contexts = {}

    first = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "tree",
        bypass_cache=True, monitoring=True,
        context_cache=routing_contexts,
    ))
    second = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "blob",
        bypass_cache=True, monitoring=True,
        context_cache=routing_contexts,
    ))

    assert first.body == "body:200"
    assert second.body == "body:200"
    assert attempts == [
        "https://a.invalid/v1/repositories/forkmesh/forkmesh/tree",
        "https://b.invalid/v1/repositories/forkmesh/forkmesh/tree",
        "https://b.invalid/v1/repositories/forkmesh/forkmesh/blob",
    ]


def test_git_transport_never_downgrades_an_outage_to_repository_not_found():
    # A clone must retry a degraded route; concluding "not found" would tell
    # git the repository is gone.
    answer, _ = _run_proxy([404, 404], operation="git-info-refs")
    assert answer.status == 503
    assert answer.payload == {"error": "mirror_unavailable"}


def test_git_transport_redirects_to_a_proven_direct_mirror_before_proxying():
    proxy, attempts = _proxy_namespace([], direct=True)
    request = SimpleNamespace(
        url=("https://forkmesh.com/forkmesh/forkmesh.git/info/refs"
             "?service=git-upload-pack"),
        headers={},
    )
    answer = asyncio.run(proxy(
        object(), request, "forkmesh", "forkmesh", "git-info-refs"))
    assert answer.status == 307
    assert answer.headers["Location"] == (
        "https://git-a.invalid/forkmesh/forkmesh.git/info/refs")
    assert answer.headers["X-ForkMesh-Transport"] == "direct-mirror"
    assert attempts == []


def test_a_signed_push_reaches_the_one_node_that_accepts_receive_pack():
    served, nodes = _merge_group_candidates(writes=("git-receive-pack",))
    assert "mirror6" not in served(0)
    for cursor in range(len(nodes)):
        assert served(cursor, operation="git-receive-pack")[0] == "mirror6"
