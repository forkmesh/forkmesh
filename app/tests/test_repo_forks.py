#!/usr/bin/env python3
"""Fork edges: the catalog field, its index pass, and the public fork list.

Same AST-extraction harness as test_repo_star.py: pull the functions under
test out of entry.py/catalog.py and stub the D1/crypto/JS primitives, so the
`forkedFrom` normalization, the repo_forks bookkeeping, and the read the repo
page's fork button issues are all exercised without a live Worker/D1.
"""

import ast
import asyncio
import re
import sys
from pathlib import Path
from urllib.parse import parse_qs, quote, unquote, urlparse


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import urls  # noqa: E402


# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
SOURCE = ((
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "node_events_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
) + "\n"
          + CATALOG.read_text(encoding="utf-8"))

FUNCS = {
    "repo_forks_handler", "_index_repo_fork_edge", "_public_repo_owner",
    "_web_fork_job_response", "_web_fork_eligible_nodes",
    "_web_fork_create", "repository_fork_jobs_handler", "_owned_nodes",
    "_account_kind",
    "_repo_is_private", "clean_string", "clean_int_series",
    "clean_optional_integer", "clean_optional_usage", "clean_logo_metadata",
    "method_name", "safe_segment", "safe_catalog_record",
}


def _load_functions(extra_globals):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="GET", url="https://forkmesh.test/x", headers=None,
                 body=None):
        self.method = method
        self.url = url
        self.headers = _Headers(headers)
        self.body = body or {}


def _harness(repositories=None, forks=None, accounts=None):
    repositories = list(
        repositories if repositories is not None
        else [{"key_bi": "bi:alice/proj", "is_private": 0}])
    forks = list(forks or [])
    accounts = dict(accounts or {})
    now = [1_700_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        rec = accounts.get(key)
        return "bi:" + key, dict(rec) if rec else None

    async def d1_first(_env, sql, *args):
        if "FROM repositories" in sql:
            for r in repositories:
                if r["key_bi"] == args[0]:
                    return {"is_private": r.get("is_private", 0)}
            return None
        if "SELECT COUNT(*) AS n FROM repo_forks" in sql:
            return {"n": sum(1 for f in forks if f["parent_bi"] == args[0])}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(_env, sql, *args):
        if "FROM repo_forks" in sql:
            parent_bi, limit = args
            rows = sorted(
                (f for f in forks if f["parent_bi"] == parent_bi),
                key=lambda f: f["created_at"], reverse=True)
            return rows[:limit]
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO repo_forks"):
            (parent_bi, fork_bi, account_bi, fork_owner, fork_name,
             created_at) = args
            for f in forks:
                if f["parent_bi"] == parent_bi and f["fork_bi"] == fork_bi:
                    f.update({"account_bi": account_bi,
                              "fork_owner": fork_owner,
                              "fork_name": fork_name})
                    return
            forks.append({"parent_bi": parent_bi, "fork_bi": fork_bi,
                          "account_bi": account_bi, "fork_owner": fork_owner,
                          "fork_name": fork_name, "created_at": created_at})
            return
        if sql.startswith("DELETE FROM repo_forks WHERE fork_bi=? AND"):
            fork_bi, parent_bi = args
            forks[:] = [f for f in forks if not (
                f["fork_bi"] == fork_bi and f["parent_bi"] != parent_bi)]
            return
        if sql.startswith("DELETE FROM repo_forks WHERE fork_bi=?"):
            forks[:] = [f for f in forks if f["fork_bi"] != args[0]]
            return
        raise AssertionError("unexpected d1_run: " + sql)

    ns = _load_functions({
        "re": re,
        "quote": quote,
        "unquote": unquote,
        "urlparse": urlparse,
        "Date": _DateStub,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.com",
        "_account_row": _account_row,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "MAX_REPO_SEGMENT": 80,
        "REPO_FORKS_RE": urls.REPO_FORKS_RE,
        "REPO_FORKS_LIST_LIMIT": 50,
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
    })
    ns["_forks"] = forks
    return ns


def _record(**overrides):
    data = {"owner": "bob", "name": "proj", "maintainer": "PUBKEY",
            "visibility": "public"}
    data.update(overrides)
    return data


# --- the catalog field -------------------------------------------------------

def test_forked_from_is_normalized_and_optional():
    ns = _harness()
    safe = ns["safe_catalog_record"]
    assert "forkedFrom" not in safe(_record())
    assert safe(_record(forkedFrom="Alice/Proj"))["forkedFrom"] == "alice/proj"
    # Junk, a bare owner, and a self-reference are all simply not fork edges.
    assert "forkedFrom" not in safe(_record(forkedFrom="alice"))
    assert "forkedFrom" not in safe(_record(forkedFrom="../../etc/passwd"))
    assert "forkedFrom" not in safe(_record(forkedFrom="bob/proj"))


# --- indexing ---------------------------------------------------------------

def test_publishing_a_fork_records_the_edge():
    ns = _harness()
    asyncio.run(ns["_index_repo_fork_edge"](
        object(), ns["safe_catalog_record"](_record(forkedFrom="alice/proj")),
        {}, "bi:bob", "bi:bob/proj"))
    assert ns["_forks"] == [{
        "parent_bi": "bi:alice/proj", "fork_bi": "bi:bob/proj",
        "account_bi": "bi:bob", "fork_owner": "bob", "fork_name": "proj",
        "created_at": 1_700_000_000_000,
    }]


def test_going_private_or_dropping_the_parent_removes_the_edge():
    seeded = [{"parent_bi": "bi:alice/proj", "fork_bi": "bi:bob/proj",
               "account_bi": "bi:bob", "fork_owner": "bob",
               "fork_name": "proj", "created_at": 1}]
    ns = _harness(forks=list(seeded))
    asyncio.run(ns["_index_repo_fork_edge"](
        object(),
        ns["safe_catalog_record"](
            _record(forkedFrom="alice/proj", visibility="private")),
        {"forkedFrom": "alice/proj"}, "bi:bob", "bi:bob/proj"))
    assert ns["_forks"] == []

    ns = _harness(forks=list(seeded))
    asyncio.run(ns["_index_repo_fork_edge"](
        object(), ns["safe_catalog_record"](_record()),
        {"forkedFrom": "alice/proj"}, "bi:bob", "bi:bob/proj"))
    assert ns["_forks"] == []


def test_repointing_a_fork_leaves_exactly_one_edge():
    ns = _harness(forks=[{"parent_bi": "bi:alice/proj",
                          "fork_bi": "bi:bob/proj", "account_bi": "bi:bob",
                          "fork_owner": "bob", "fork_name": "proj",
                          "created_at": 1}])
    asyncio.run(ns["_index_repo_fork_edge"](
        object(), ns["safe_catalog_record"](_record(forkedFrom="carol/proj")),
        {"forkedFrom": "alice/proj"}, "bi:bob", "bi:bob/proj"))
    assert [f["parent_bi"] for f in ns["_forks"]] == ["bi:carol/proj"]


# --- the public list --------------------------------------------------------

def _forks_request(owner="alice", repo="proj"):
    return _Request(
        "GET", "https://forkmesh.test/api/repo/%s/%s/forks" % (owner, repo))


def test_forks_list_is_public_and_newest_first():
    ns = _harness(
        repositories=[
            {"key_bi": "bi:alice/proj", "is_private": 0},
            {"key_bi": "bi:bob/proj", "is_private": 0},
            {"key_bi": "bi:carol/proj", "is_private": 0},
        ],
        forks=[
            {"parent_bi": "bi:alice/proj", "fork_bi": "bi:bob/proj",
             "account_bi": "bi:bob", "fork_owner": "bob", "fork_name": "proj",
             "created_at": 10},
            {"parent_bi": "bi:alice/proj", "fork_bi": "bi:carol/proj",
             "account_bi": "bi:carol", "fork_owner": "carol",
             "fork_name": "proj", "created_at": 20},
        ],
        accounts={"carol": {"status": "active", "avatar_png": "PNG"}})
    resp = asyncio.run(ns["repo_forks_handler"](
        object(), _forks_request(), "alice", "proj"))
    assert resp["status"] == 200
    assert resp["data"]["ok"] is True
    assert resp["data"]["count"] == 2
    assert [f["owner"] for f in resp["data"]["forks"]] == ["carol", "bob"]
    assert resp["data"]["forks"][0]["url"] == "/carol/proj"
    # Portraits only for a live public user account; "bob" has no account row.
    assert resp["data"]["forks"][0]["avatarPng"] == "PNG"
    assert resp["data"]["forks"][0]["profileUrl"] == \
        "https://forkmesh.com/@carol"
    assert "avatarPng" not in resp["data"]["forks"][1]


def test_forks_list_leaves_out_private_forks():
    ns = _harness(
        repositories=[
            {"key_bi": "bi:alice/proj", "is_private": 0},
            {"key_bi": "bi:bob/proj", "is_private": 0},
            {"key_bi": "bi:carol/secret", "is_private": 1},
        ],
        forks=[
            {"parent_bi": "bi:alice/proj", "fork_bi": "bi:bob/proj",
             "account_bi": "bi:bob", "fork_owner": "bob", "fork_name": "proj",
             "created_at": 10},
            {"parent_bi": "bi:alice/proj", "fork_bi": "bi:carol/secret",
             "account_bi": "bi:carol", "fork_owner": "carol",
             "fork_name": "secret", "created_at": 20},
        ])
    resp = asyncio.run(ns["repo_forks_handler"](
        object(), _forks_request(), "alice", "proj"))
    assert [f["owner"] for f in resp["data"]["forks"]] == ["bob"]
    assert resp["data"]["count"] == 1


def test_forks_list_404s_for_a_private_repo():
    ns = _harness(repositories=[{"key_bi": "bi:alice/proj", "is_private": 1}])
    resp = asyncio.run(ns["repo_forks_handler"](
        object(), _forks_request(), "alice", "proj"))
    assert resp["status"] == 404


def test_forks_route_rejects_unsupported_methods():
    ns = _harness()
    resp = asyncio.run(ns["repo_forks_handler"](
        object(), _Request("PUT", "https://forkmesh.test/api/repo/alice/proj/forks"),
        "alice", "proj"))
    assert resp["status"] == 405


# --- website -> managed mirror durable job ---------------------------------

def _web_fork_harness(runtime_mode="headless"):
    now = [1_700_000_000_000]
    jobs = []
    notifications = []
    accounts = {
        "dana": {"name": "dana", "kind": "user", "status": "active",
                 "nodes": ["mirror9"]},
        "mirror9": {"name": "mirror9", "kind": "node", "status": "active",
                    "owner": "dana"},
    }
    repositories = [
        {"key_bi": "bi:alice/proj", "owner_bi": "bi:alice",
         "is_private": 0,
         "data": {"owner": "alice", "name": "proj",
                  "visibility": "public"}},
        {"key_bi": "bi:mirror9/seed", "owner_bi": "bi:mirror9",
         "is_private": 0,
         "data": {"owner": "mirror9", "name": "seed",
                  "visibility": "public", "runtimeMode": runtime_mode}},
    ]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def encrypt_row(_env, value):
        return dict(value)

    async def decrypt_row(_env, value, key=None):
        del key
        return dict(value) if isinstance(value, dict) else None

    async def bounded_json_request(request):
        return dict(request.body)

    async def account_session(_env, _request, data=None):
        if (data or {}).get("sessionToken") != "session":
            return "", None
        return "bi:dana", dict(accounts["dana"])

    async def account_row(_env, name):
        name = str(name or "").lower()
        rec = accounts.get(name)
        return "bi:" + name, dict(rec) if rec else None

    async def d1_first(_env, sql, *args):
        if "FROM repositories WHERE key_bi=?" in sql:
            return next((dict(row) for row in repositories
                         if row["key_bi"] == args[0]), None)
        if "COUNT(*) AS n FROM repository_fork_jobs" in sql:
            return {"n": sum(1 for row in jobs
                             if row["actor_bi"] == args[0]
                             and row["status"] == "pending")}
        if "FROM repository_fork_jobs WHERE target_repo_bi=?" in sql:
            return next((dict(row) for row in jobs
                         if row["target_repo_bi"] == args[0]), None)
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_all(_env, sql, *args):
        if sql.startswith("SELECT owner_bi,data FROM repositories"):
            wanted = set(args)
            return [{"owner_bi": row["owner_bi"], "data": row["data"]}
                    for row in repositories if row["owner_bi"] in wanted]
        if sql.startswith("SELECT key_bi,data FROM repositories"):
            wanted = set(args)
            return [{"key_bi": row["key_bi"], "data": row["data"]}
                    for row in repositories if row["key_bi"] in wanted]
        if "FROM repository_fork_jobs WHERE target_repo_bi IN" in sql:
            wanted = set(args)
            return [dict(row) for row in jobs
                    if row["target_repo_bi"] in wanted]
        if "status='pending'" in sql and "ORDER BY created_at" in sql:
            target_node_bi, limit = args
            return [dict(row) for row in jobs
                    if row["target_node_bi"] == target_node_bi
                    and row["status"] == "pending"][:limit]
        if "SELECT request_id FROM repository_fork_jobs" in sql:
            target_node_bi, *wanted = args
            return [{"request_id": row["request_id"]} for row in jobs
                    if row["target_node_bi"] == target_node_bi
                    and row["status"] == "pending"
                    and row["request_id"] in wanted]
        raise AssertionError("unexpected d1_all: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT INTO repository_fork_jobs"):
            (request_id, target_node_bi, target_repo_bi, source_repo_bi,
             actor_bi, data, created_at, updated_at) = args
            if any(row["target_repo_bi"] == target_repo_bi for row in jobs):
                raise RuntimeError("UNIQUE target")
            jobs.append({
                "request_id": request_id,
                "target_node_bi": target_node_bi,
                "target_repo_bi": target_repo_bi,
                "source_repo_bi": source_repo_bi,
                "actor_bi": actor_bi,
                "data": data,
                "status": "pending",
                "created_at": created_at,
                "updated_at": updated_at,
            })
            return
        if sql.startswith("UPDATE repository_fork_jobs SET status='completed'"):
            _, _, target_node_bi, *ids = args
            for row in jobs:
                if (row["target_node_bi"] == target_node_bi
                        and row["request_id"] in ids
                        and row["status"] == "pending"):
                    row["status"] = "completed"
            return
        raise AssertionError("unexpected d1_run: " + sql)

    async def notify(_env, owner, repo, topic, state=""):
        notifications.append((owner, repo, topic, state))

    async def authorize(_env, _request, owner):
        return "PUBKEY" if owner == "mirror9" else ""

    async def hosted_fork_create(_env, _request, _owner, _public, _repo):
        # Not a hosted parent: the managed-mirror path runs.
        return None

    ns = _load_functions({
        "hosted_fork_create": hosted_fork_create,
        "re": re,
        "parse_qs": parse_qs,
        "quote": quote,
        "unquote": unquote,
        "urlparse": urlparse,
        "Date": _DateStub,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "encrypt_row": encrypt_row,
        "decrypt_row": decrypt_row,
        "bounded_json_request": bounded_json_request,
        "_account_session_record": account_session,
        "_account_row": account_row,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "notify_repo_host": notify,
        "_authorized_owner_signing_key": authorize,
        "_random_bytes": lambda size: bytes(range(size)),
        "_ap_origin": lambda _env, _request=None: "https://forkmesh.test",
        "_org_repo_node": lambda *_args: None,
        "MAX_NODE_NAME": 80,
        "MAX_REPO_SEGMENT": 80,
        "REPO_FORKS_RE": urls.REPO_FORKS_RE,
        "REPO_FORKS_LIST_LIMIT": 24,
        "WEB_FORK_PENDING_PER_ACCOUNT": 20,
        "WEB_FORK_JOB_BATCH": 20,
        "WEB_FORK_REQUEST_ID_RE": re.compile(r"^[0-9a-f]{36}$"),
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._:-]+$"),
    })
    ns["_jobs"] = jobs
    ns["_notifications"] = notifications
    return ns


def _create_fork_request():
    return _Request(
        "POST", "https://forkmesh.test/api/repo/alice/proj/forks",
        body={"sessionToken": "session"})


def test_website_fork_is_durable_for_an_offline_headless_mirror():
    ns = _web_fork_harness()
    response = asyncio.run(ns["repo_forks_handler"](
        object(), _create_fork_request(), "alice", "proj"))
    assert response["status"] == 202
    assert response["data"]["queued"] is True
    assert response["data"]["fork"]["owner"] == "mirror9"
    assert response["data"]["fork"]["url"] == "/mirror9/proj"
    assert len(ns["_jobs"]) == 1
    assert ns["_jobs"][0]["data"]["sourceUrl"] == \
        "https://forkmesh.test/alice/proj"
    # There is deliberately no live-endpoint check in the harness or handler:
    # the durable row and best-effort wake-up are enough while mirror9 is down.
    assert ns["_notifications"] == [("mirror9", "proj", "forks", "")]


def test_website_fork_double_click_is_idempotent():
    ns = _web_fork_harness()
    first = asyncio.run(ns["repo_forks_handler"](
        object(), _create_fork_request(), "alice", "proj"))
    second = asyncio.run(ns["repo_forks_handler"](
        object(), _create_fork_request(), "alice", "proj"))
    assert first["data"]["fork"]["requestId"] == \
        second["data"]["fork"]["requestId"]
    assert len(ns["_jobs"]) == 1


def test_website_fork_requires_a_managed_mirror_capability():
    ns = _web_fork_harness(runtime_mode="desktop")
    response = asyncio.run(ns["repo_forks_handler"](
        object(), _create_fork_request(), "alice", "proj"))
    assert response["status"] == 409
    assert response["data"]["error"] == "headless_mirror_required"
    assert ns["_jobs"] == []


def test_managed_mirror_signed_drain_and_exact_ack():
    ns = _web_fork_harness()
    created = asyncio.run(ns["repo_forks_handler"](
        object(), _create_fork_request(), "alice", "proj"))
    request_id = created["data"]["fork"]["requestId"]
    base = "https://forkmesh.test/api/repository-forks?owner=mirror9&ts=1&sig=x"
    drained = asyncio.run(ns["repository_fork_jobs_handler"](
        object(), _Request("GET", base)))
    assert drained["status"] == 200
    assert [row["requestId"] for row in drained["data"]["pending"]] == \
        [request_id]
    assert drained["data"]["pending"][0]["sourceUrl"] == \
        "https://forkmesh.test/alice/proj"

    acked = asyncio.run(ns["repository_fork_jobs_handler"](
        object(), _Request("DELETE", base + "&ids=" + request_id)))
    assert acked["data"]["acknowledged"] == [request_id]
    assert ns["_jobs"][0]["status"] == "completed"
    empty = asyncio.run(ns["repository_fork_jobs_handler"](
        object(), _Request("GET", base)))
    assert empty["data"]["pending"] == []


# --- desktop/relay parity ----------------------------------------------------
# The desktop signs the record it publishes and the relay re-normalizes it
# before verifying, so the two normalizers must agree on `forkedFrom` down to
# whether the key exists at all: an empty string on one side and an absent key
# on the other is a catalog-v2 signature mismatch, i.e. every publish rejected.

QT_SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"


def test_desktop_passes_fork_provenance_to_the_managed_publisher():
    control = (QT_SRC / "MainWindowControlNode.cpp").read_text(encoding="utf-8")
    assert 'repository.insert(QStringLiteral("forkedFrom"), forkParent);' in control
    assert '"^[A-Za-z0-9._:-]{1,80}/[A-Za-z0-9._:-]{1,80}$"' in control

    root = QT_SRC.parents[1]
    gateway = (root / "tools" / "mirror_gateway.py").read_text(encoding="utf-8")
    publisher = (root / "server" / "catalog.go").read_text(encoding="utf-8")
    assert '"forkedFrom",' in gateway
    assert 'record["forkedFrom"] = strings.ToLower(repo.ForkedFrom)' in publisher


def test_website_fork_handoff_uses_one_mesh_cache_before_forking():
    main = (QT_SRC / "main.cpp").read_text(encoding="utf-8")
    chat = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_CHAT_SOURCES
    )
    repos = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    single = (QT_SRC / "SingleInstance.cpp").read_text(encoding="utf-8")

    assert 'url.host() != QLatin1String("fork")' in main
    assert "openRepositoryForkFromSystemLink(target)" in main
    assert "hostedCloneUrl(owner, name)" in chat
    handoff = chat[
        chat.index("void MainWindow::forkNetworkRepo(")
        : chat.index("void MainWindow::mirrorNetworkRepo(")
    ]
    assert "m_pendingForkRepoKey" in handoff
    assert "openNetworkRepo(owner, name, cloneUrl, isPrivate);" in handoff
    assert "forkCurrentRepo();" not in handoff
    assert "completePendingRepoFork(preview)" in handoff
    completion = repos[
        repos.index("bool MainWindow::completePendingRepoFork(")
        : repos.index("void MainWindow::startSyncFetch(")
    ]
    assert "m_syncingRepos.contains(index)" in completion
    assert 'filePath(QStringLiteral("HEAD"))' in completion
    assert "forkCurrentRepo();" in completion
    # A validated 512-byte fork URI survives handoff to an already-running app.
    assert "target.left(4096)" in single
    assert "socket->read(4128)" in single


def test_fork_borrows_the_upstream_object_store():
    detail = (QT_SRC / "MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
    fork = detail[detail.index("void MainWindow::forkCurrentRepo"):]
    fork = fork[:fork.index("\nnamespace {")]
    # --no-local is what makes --reference meaningful: without it a local clone
    # hardlinks the whole object directory in and the reference is pointless.
    assert '"--no-local"' in fork
    assert '"--reference-if-able"' in fork
    assert 'fork.forkedFrom = (src.owner.trimmed()' in fork
    # The working checkout borrows from the fork mirror too; only working-tree
    # files are materialized a second time, not the Git object database.
    assert fork.count('QStringLiteral("--reference-if-able")') >= 2
    # Deleting an upstream must re-materialize what its forks were borrowing.
    repos = "".join(
        (QT_SRC / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    assert "detachForksBorrowingMirror(path);" in repos
    assert repos.index("detachForksBorrowingMirror(path);") < repos.index(
        "forkmesh::safedelete::removeTree(path,")


def test_a_repository_that_never_declared_a_parent_writes_nothing():
    # The overwhelmingly common publish. It must not spend a D1 write clearing
    # an edge that was never there. Record the calls rather than raising: the
    # indexer swallows exceptions by design, so a raising stub proves nothing.
    ns = _harness()
    calls = []

    async def record_call(_env, sql, *args):
        calls.append(sql)

    ns["d1_run"] = record_call
    asyncio.run(ns["_index_repo_fork_edge"](
        object(), ns["safe_catalog_record"](_record()), {},
        "bi:bob", "bi:bob/proj"))
    assert calls == []
    # ...but a repository that *was* a fork and stopped being one still clears.
    asyncio.run(ns["_index_repo_fork_edge"](
        object(), ns["safe_catalog_record"](_record()),
        {"forkedFrom": "alice/proj"}, "bi:bob", "bi:bob/proj"))
    assert calls == ["DELETE FROM repo_forks WHERE fork_bi=?"]
