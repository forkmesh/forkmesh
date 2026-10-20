import ast
import asyncio
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
sys.path.insert(0, str(ROOT / "host"))

from forkmesh_host.workers import Response  # noqa: E402

sys.path.insert(0, str(ROOT / "src"))
import instance_features  # noqa: E402


def _namespace():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"GIT_ERROR_HELP_URL", "GIT_ERROR_MESSAGES",
              "GIT_ERROR_KEPT_HEADERS", "_git_error_as_text"}
    body = [
        node for node in tree.body
        if (isinstance(node, ast.AsyncFunctionDef) and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(getattr(t, "id", "") in wanted for t in node.targets))
    ]
    assert len(body) == len(wanted)
    namespace = {
        "json": json,
        "Response": Response,
        "EXPECTED_DEGRADED_HEADER": "x-forkmesh-expected-degraded",
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
        "js_nullish": lambda value: value is None,
        "instance_features": instance_features,
    }
    exec(compile(ast.Module(body=body, type_ignores=[]), str(ENTRY), "exec"),
         namespace)
    return namespace


NS = _namespace()


def _convert(response):
    return asyncio.run(NS["_git_error_as_text"](response))


def _json(payload, status, headers=None):
    return Response(json.dumps(payload), status=status, headers={
        "content-type": "application/json; charset=utf-8", **(headers or {})})


def test_mirror_unavailable_becomes_plain_text_git_prints():
    converted = _convert(_json(
        {"error": "mirror_unavailable"}, 503,
        {"cache-control": "no-store",
         "x-forkmesh-expected-degraded": "1"}))
    assert converted.status == 503
    assert converted.headers.get("content-type") == "text/plain; charset=utf-8"
    assert converted.headers.get("cache-control") == "no-store"
    assert converted.headers.get("x-forkmesh-expected-degraded") == "1"
    text = asyncio.run(converted.text())
    assert text.startswith("ForkMesh could not reach a mirror")
    assert "Help: https://forkmesh.com/docs/git-errors/" in text
    assert "{" not in text


def test_basic_challenge_keeps_www_authenticate():
    converted = _convert(_json(
        {"error": "push_unauthorized"}, 401,
        {"WWW-Authenticate": 'Basic realm="forkmesh"'}))
    assert converted.status == 401
    assert converted.headers.get("www-authenticate") == 'Basic realm="forkmesh"'
    assert "push credential" in asyncio.run(converted.text())


def test_server_message_or_hint_wins_over_the_table():
    quota = _convert(_json(
        {"error": "repository_quota_exceeded",
         "message": "This repository has reached the 500 MB limit."}, 413))
    assert asyncio.run(quota.text()).startswith(
        "This repository has reached the 500 MB limit.")
    protocol = _convert(_json(
        {"error": "unsupported_git_protocol",
         "hint": "Use Git protocol version 0, 1, or 2."}, 400))
    assert asyncio.run(protocol.text()).startswith("Use Git protocol version")


def test_unknown_code_names_status_and_code():
    converted = _convert(_json({"error": "weird_thing"}, 409))
    assert asyncio.run(converted.text()).startswith(
        "ForkMesh answered HTTP 409 (weird_thing).")


def test_success_and_non_json_errors_pass_through():
    ok = Response("001e# service=git-upload-pack\n", status=200, headers={
        "content-type": "application/x-git-upload-pack-advertisement"})
    assert _convert(ok) is ok
    plain = Response("request too large", status=413,
                     headers={"content-type": "text/plain"})
    assert _convert(plain) is plain


def test_every_info_refs_route_is_wrapped():
    source = APP_RUNTIME.read_text(encoding="utf-8")
    start = source.index("git_info = GIT_INFO_RE.match(url.path)")
    block = source[start:source.index("git_pack = GIT_PACK_RE", start)]
    assert block.count("return await _git_error_as_text(") == 3
    assert block.count("return await") == 3


def test_instance_git_errors_name_the_instance_without_the_forkmesh_help_link():
    from types import SimpleNamespace
    response = _json({"error": "not_found"}, 404)
    converted = asyncio.run(NS["_git_error_as_text"](
        response, SimpleNamespace(INSTANCE_NAME="Acme Code")))
    text = asyncio.run(converted.text())
    assert text == ("Acme Code has no public repository at this address. "
                    "Check the owner and repository name.\n")
    plain = asyncio.run(NS["_git_error_as_text"](
        _json({"error": "not_found"}, 404), SimpleNamespace()))
    assert "forkmesh.com/docs/git-errors/" in asyncio.run(plain.text())
    source = APP_RUNTIME.read_text(encoding="utf-8")
    start = source.index("git_info = GIT_INFO_RE.match(url.path)")
    block = source[start:source.index("git_pack = GIT_PACK_RE", start)]
    assert block.count("self.env)") >= 3
