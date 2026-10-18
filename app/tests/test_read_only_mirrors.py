import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

import catalog
import read_only_mirrors as mirrors

SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME_TEXT = (SRC / "app_runtime.py").read_text(encoding="utf-8")
UPSTREAM = "https://forkmesh.com/forkmesh/forkmesh"
COMMIT = "fafe38d7eec975f77e9686f8396714ef5ed781f8"


def _function(text, name):
    for node in ast.walk(ast.parse(text)):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and \
                node.name == name:
            return node
    raise AssertionError(name)


def _load(names, namespace):
    module = ast.Module(
        body=[_function(ENTRY_TEXT, name) for name in names], type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), "entry.py", "exec"),
         namespace)
    return namespace


def test_node_signed_record_fields_describe_the_mirror():
    record = {"mirrorOf": UPSTREAM, "lastSyncedAt": "1792285262771",
              "upstreamCommit": COMMIT, "lastSync": "5", "commit": "f" * 40}
    assert mirrors.mirror_fields(record) == {
        "mirrorOf": UPSTREAM, "readOnly": True,
        "lastSyncedAt": 1792285262771, "upstreamCommit": COMMIT}


def test_instance_platform_copy_is_a_mirror_without_node_fields():
    record = {"lastSync": "1792285262000", "commit": COMMIT}
    fields = mirrors.mirror_fields(
        record, UPSTREAM + ".git", "forkmesh", "forkmesh", "forkmesh")
    assert fields == {"mirrorOf": UPSTREAM, "readOnly": True,
                      "lastSyncedAt": 1792285262000, "upstreamCommit": COMMIT}
    assert mirrors.mirror_fields(
        record, UPSTREAM, "forkmesh", "acme", "forkmesh")["readOnly"] is False
    assert mirrors.mirror_fields(
        record, UPSTREAM, "", "forkmesh", "forkmesh")["readOnly"] is False
    assert mirrors.mirror_fields(
        record, "http://forkmesh.com/x/y", "forkmesh", "forkmesh",
        "forkmesh")["readOnly"] is False
    assert mirrors.mirror_fields(None) == mirrors.not_a_mirror()
    assert mirrors.upstream_label(UPSTREAM) == "forkmesh/forkmesh"


def test_only_repository_writes_are_refused():
    base = "/api/repo/forkmesh/forkmesh/"
    for method, path in (
            ("POST", base + "file-edit"), ("POST", base + "branches/ops"),
            ("DELETE", base + "branches/ops"), ("POST", base + "pulls"),
            ("POST", base + "pulls/12/merge"), ("POST", base + "pulls/3/update")):
        assert mirrors.is_write_request(method, path), (method, path)
    for method, path in (
            ("GET", base + "branches/ops"), ("GET", base + "pulls"),
            ("POST", base + "branches/ops/results"), ("POST", base + "about"),
            ("POST", base + "forks"), ("POST", base + "merge-preview"),
            ("HEAD", base + "file-edit"), ("POST", base + "issues")):
        assert not mirrors.is_write_request(method, path), (method, path)
    body = mirrors.refusal_body(UPSTREAM)
    assert body["error"] == "read_only_mirror" and body["mirrorOf"] == UPSTREAM
    assert UPSTREAM in body["message"] and "fork" in body["message"]


def test_catalog_keeps_the_signed_mirror_extension_exactly():
    data = {
        "owner": "forkmesh", "name": "forkmesh", "visibility": "public",
        "maintainer": "A" * 43, "stateHash": "b" * 64,
        "mirrorOf": UPSTREAM, "upstreamCommit": COMMIT,
        "lastSyncedAt": "1792285262771",
    }
    record = catalog.safe_catalog_record(data)
    assert record["mirrorOf"] == UPSTREAM
    assert record["upstreamCommit"] == COMMIT
    assert record["lastSyncedAt"] == "1792285262771"
    plain = catalog.safe_catalog_record(
        {key: value for key, value in data.items()
         if key not in ("mirrorOf", "upstreamCommit", "lastSyncedAt")})
    assert not {"mirrorOf", "upstreamCommit", "lastSyncedAt"} & set(plain)
    for bad in ("http://forkmesh.com/a/b", UPSTREAM + "\n",
                "https://forkmesh.com/a?b=1", "https://u@forkmesh.com/a"):
        tampered = catalog.safe_catalog_record(dict(data, mirrorOf=bad))
        assert "mirrorOf" not in tampered and "lastSyncedAt" not in tampered
    odd = catalog.safe_catalog_record(
        dict(data, lastSyncedAt="0123", upstreamCommit="xyz"))
    assert "lastSyncedAt" not in odd and "upstreamCommit" not in odd


def _runtime_namespace(record, upstream=UPSTREAM, flagship="forkmesh"):
    async def repo_catalog_record(_env, owner, repo):
        return record

    def json_response(data, status=200, cache_control=None):
        return {"status": status, "data": data, "cache": cache_control}

    class Response:
        def __init__(self, body, status=200, headers=None):
            self.body, self.status, self.headers = body, status, headers

    return _load(
        ["_read_only_mirror_fields", "read_only_mirror_refusal",
         "read_only_mirror_push_refusal"],
        {"_read_only_mirrors": mirrors,
         "_repo_catalog_record": repo_catalog_record,
         "_instance_solo_flagship_node": lambda env: flagship,
         "method_name": lambda request: request.method,
         "json_response": json_response, "Response": Response})


def test_write_routes_on_a_mirror_answer_a_clear_refusal():
    env = SimpleNamespace(INSTANCE_UPSTREAM=UPSTREAM)
    ns = _runtime_namespace({"commit": COMMIT})
    request = SimpleNamespace(method="POST")
    refused = asyncio.run(ns["read_only_mirror_refusal"](
        env, request, "forkmesh", "forkmesh",
        "/api/repo/forkmesh/forkmesh/file-edit"))
    assert refused["status"] == 403 and refused["cache"] == "no-store"
    assert refused["data"]["error"] == "read_only_mirror"
    assert asyncio.run(ns["read_only_mirror_refusal"](
        env, SimpleNamespace(method="GET"), "forkmesh", "forkmesh",
        "/api/repo/forkmesh/forkmesh/file-edit")) is None
    assert asyncio.run(ns["read_only_mirror_refusal"](
        env, request, "acme", "bolt",
        "/api/repo/acme/bolt/file-edit")) is None
    plain = _runtime_namespace({"commit": COMMIT}, flagship="")
    assert asyncio.run(plain["read_only_mirror_refusal"](
        SimpleNamespace(), request, "forkmesh", "forkmesh",
        "/api/repo/forkmesh/forkmesh/file-edit")) is None


def test_git_push_to_a_mirror_is_refused_in_words_git_prints():
    env = SimpleNamespace(INSTANCE_UPSTREAM=UPSTREAM)
    ns = _runtime_namespace({"mirrorOf": UPSTREAM}, upstream="", flagship="")
    refused = asyncio.run(ns["read_only_mirror_push_refusal"](
        env, "forkmesh", "forkmesh"))
    assert refused.status == 403
    assert refused.headers["content-type"].startswith("text/plain")
    assert "read-only mirror of " + UPSTREAM in refused.body
    assert asyncio.run(ns["read_only_mirror_push_refusal"](
        SimpleNamespace(), "forkmesh", "")) is None
    hosted = _runtime_namespace({"commit": COMMIT}, flagship="")
    assert asyncio.run(hosted["read_only_mirror_push_refusal"](
        SimpleNamespace(), "acme", "bolt")) is None


def test_routes_check_the_mirror_before_dispatching_writes():
    route = ast.unparse(_function(APP_RUNTIME_TEXT, "_route"))
    alias = route.index("org_alias_rewrite(")
    guard = route.index("read_only_mirror_refusal(")
    assert alias < guard < route.index("repository_file_edits.route(")
    push = ast.unparse(_function(APP_RUNTIME_TEXT, "_git_push"))
    assert push.index("read_only_mirror_push_refusal(") < push.index(
        "_https_mirror_push_proxy(")
    about = ast.unparse(_function(ENTRY_TEXT, "_repo_about_public"))
    assert "_read_only_mirror_fields(env, owner, repo, rec or {})" in about


def test_about_closes_collaboration_on_a_mirror_only():
    open_features = {"issues": True, "pulls": True, "discussions": True}
    mirror = mirrors.mirror_fields({"mirrorOf": UPSTREAM, "commit": COMMIT})
    about = mirrors.about_fields(open_features, mirror)
    assert about["features"] == {
        "issues": False, "pulls": False, "discussions": False}
    assert about["mirrorOf"] == UPSTREAM and about["readOnly"] is True
    assert about["upstreamCommit"] == COMMIT
    assert open_features["issues"] is True
    plain = mirrors.about_fields(open_features, mirrors.not_a_mirror())
    assert plain["features"] == open_features and plain["readOnly"] is False
    handler = _function(ENTRY_TEXT, "_repo_about_public")
    source = ast.get_source_segment(ENTRY_TEXT, handler)
    assert "_read_only_mirrors.about_fields(" in source
    assert "_read_only_mirror_fields(env, owner, repo, rec or {})" in source
