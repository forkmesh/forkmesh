import ast
import asyncio
import importlib.util
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "src" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ap_threads = load_module("activitypub_threads")


def load_function(name, namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == name
    ]
    assert selected
    exec(
        compile(
            ast.fix_missing_locations(
                ast.Module(body=selected, type_ignores=[])
            ),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    return namespace[name]


class Response:
    def __init__(self, data, status=200, cache_control=None):
        self.data = data
        self.status = status
        self.cache_control = cache_control


class Request:
    method = "GET"
    url = (
        "https://forkmesh.test/api/repo/forkmesh/forkmesh/fedi-comments"
        "?kind=discussion&number=9"
    )


def record(remote_id, parent="", instance="lemmy.example", ts=1):
    return {
        "schema": ap_threads.THREAD_SCHEMA,
        "storageNamespace": ap_threads.STORAGE_NAMESPACE,
        "remoteId": remote_id,
        "parentRemoteId": parent,
        "body": "public remote reply",
        "author": "alice",
        "authorName": "Alice",
        "authorUrl": f"https://{instance}/u/alice",
        "backlink": remote_id,
        "published": "2026-07-23T00:00:00Z",
        "updated": "",
        "receivedAt": ts,
        "sourceInstance": instance,
        "sourceSoftware": "lemmy",
        "lifecycle": "active",
        "provenance": {
            "source": "activitypub",
            "instance": instance,
            "software": "lemmy",
            "backlink": remote_id,
            "nativeEvent": False,
            "nativeEventId": "",
        },
    }


def test_client_projection_keeps_nesting_and_refilters_instance_blocks():
    root = record("https://lemmy.example/comment/1", ts=1)
    child = record(
        "https://lemmy.example/comment/2",
        parent="https://lemmy.example/comment/1",
        ts=2,
    )
    blocked = record("https://blocked.example/comment/3", instance="blocked.example")

    async def ensure_schema(env):
        return None

    async def private(env, owner, repo):
        return False

    async def blind_index(env, value):
        return "context-bi"

    # Rows carry an opaque ciphertext blob, as production does — the handler
    # memoizes decrypted replies by that exact string, so the harness must not
    # hand it an already-decrypted dict.
    blobs = {"blob-1": root, "blob-2": child, "blob-3": blocked}

    async def d1_all(env, sql, *args):
        assert "ap_comments" in sql
        return [
            {"data": "blob-1", "ts": 1, "lifecycle": "active"},
            {"data": "blob-2", "ts": 2, "lifecycle": "active"},
            {"data": "blob-3", "ts": 3, "lifecycle": "active"},
        ]

    decrypts = []

    async def decrypt_row(env, value, key=None):
        decrypts.append(value)
        return blobs.get(value)

    async def domain_blocked(env, host):
        return host == "blocked.example"

    async def data_key(env):
        return "aes-key"

    puts = []

    async def edge_cache_match(cache_key):
        return None

    async def edge_cache_put(cache_key, response):
        puts.append((cache_key, response))

    def json_response(data, status=200, cache_seconds=None,
                      cache_control=None):
        if cache_control is None and cache_seconds is not None:
            cache_control = "public, max-age=%d" % cache_seconds
        return Response(data, status, cache_control)

    namespace = {
        "method_name": lambda request: request.method,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "_repo_is_private": private,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "quote": quote,
        "clean_string": lambda value, limit: str(value)[:limit],
        "blind_index": blind_index,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "_data_key": data_key,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "_FEDI_COMMENT_DECRYPT_MEMO": {},
        "FEDI_COMMENT_DECRYPT_MEMO_MAX": 2048,
        "FEDI_COMMENT_ROW_LIMIT": 200,
        "FEDI_COMMENTS_CACHE_TTL": 30,
        "_ap_domain_blocked": domain_blocked,
        "ap_threads": ap_threads,
        "ap": type(
            "AP",
            (),
            {
                "context_key": staticmethod(
                    lambda owner, repo, kind, ref: (
                        f"{owner}/{repo}#{kind}#{ref}"
                    )
                )
            },
        ),
        "AP_FEDI_KINDS": ("issue", "pull", "discussion"),
    }
    handler = load_function("fedi_comments_handler", namespace)
    response = asyncio.run(handler(None, Request(), "forkmesh", "forkmesh"))

    assert response.status == 200
    assert response.cache_control == "public, max-age=30"
    # The built response is offered to the colo cache, keyed by the thread.
    assert len(puts) == 1
    assert puts[0][0] == (
        "https://forkmesh.internal/api/repo/forkmesh/forkmesh/fedi-comments"
        "?kind=discussion&ref=9"
    )
    # A second read of the same thread reuses the memoized plaintext instead of
    # replaying an AES-GCM decrypt per row — the Worker-CPU (Cloudflare 1102)
    # guard for busy threads.
    decrypts.clear()
    asyncio.run(handler(None, Request(), "forkmesh", "forkmesh"))
    assert decrypts == []
    comments = response.data["comments"]
    assert [item["remoteId"] for item in comments] == [
        "https://lemmy.example/comment/1",
        "https://lemmy.example/comment/2",
    ]
    assert [item["depth"] for item in comments] == [0, 1]
    assert all(item["nativeEvent"] is False for item in comments)
    assert all(
        item["provenance"]["nativeEvent"] is False for item in comments
    )
