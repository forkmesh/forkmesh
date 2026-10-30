#!/usr/bin/env python3
"""Direct-HTTPS Git transport regression checks."""

import ast
import gzip
import io
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
GIT_HTTP = ROOT / "src" / "git_http.py"


def _load_helper(name, namespace=None):
    tree = ast.parse(GIT_HTTP.read_text(encoding="utf-8"))
    function = next(
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == name)
    scope = dict(namespace or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=[function], type_ignores=[])),
        str(GIT_HTTP), "exec"), scope)
    return scope[name]


decode_git_request_body = _load_helper(
    "decode_git_request_body", {"gzip": gzip, "io": io})
git_advert_cache_key = _load_helper("git_advert_cache_key")


def _method_source(class_name, method_name):
    tree = ast.parse((
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
))
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == class_name:
            for child in node.body:
                if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                        and child.name == method_name:
                    return ast.unparse(child)
    raise AssertionError(f"{class_name}.{method_name} not found")


def _function_source(name):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) \
                and node.name == name:
            return ast.unparse(node)
    raise AssertionError(f"{name} not found")


def test_plain_and_gzip_git_request_decoding_is_bounded():
    payload = b"0032want deadbeef\n00000009done\n"
    assert decode_git_request_body(payload, "") == payload
    assert decode_git_request_body(payload, "identity") == payload
    assert decode_git_request_body(gzip.compress(payload), " GZip ") == payload
    with pytest.raises(ValueError, match="invalid gzip"):
        decode_git_request_body(b"not gzip", "gzip")
    with pytest.raises(ValueError, match="unsupported"):
        decode_git_request_body(payload, "br")
    with pytest.raises(ValueError, match="too large"):
        decode_git_request_body(gzip.compress(b"12345"), "gzip", max_bytes=4)


def test_git_advert_cache_key_rotates_with_attested_state():
    first = git_advert_cache_key("Forkmesh", "Forkmesh", "ABC123")
    assert first == (
        "https://forkmesh.internal/git-advert/forkmesh/forkmesh/"
        "git-upload-pack/abc123")
    assert git_advert_cache_key("forkmesh", "forkmesh", "abc123") == first
    assert git_advert_cache_key("forkmesh", "forkmesh", "def456") != first
    assert git_advert_cache_key("forkmesh", "forkmesh", "") is None
    assert git_advert_cache_key("", "forkmesh", "abc123") is None


def test_public_clone_uses_signed_direct_https_and_sticky_selection():
    source = ENTRY.read_text(encoding="utf-8")
    tree = ast.parse(source)
    route = _method_source("Default", "_route")
    assert "_https_mirror_proxy(" in route
    assert "'git-info-refs'" in route
    assert "'git-upload-pack'" in route

    proxy = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_https_mirror_proxy"))
    assert "https_routing.request_message" in proxy
    assert "ed25519_sign" in proxy
    assert "JsResponse.new(upstream.body" in proxy
    assert "js_fetch_until_response" in proxy
    assert "operation == 'git-upload-pack'" in proxy
    assert "request.headers.get('git-protocol')" in proxy
    assert "headers['Git-Protocol'] = git_protocol" in proxy
    assert "unsupported_git_protocol" in proxy
    assert "HTTPS_MIRROR_GIT_INITIAL_RESPONSE_TIMEOUT_SECONDS" in proxy
    assert "https_routing.direct_git_target_url" in proxy
    assert "X-ForkMesh-Transport" in proxy
    assert "direct-mirror" in proxy
    assert "status=307" in proxy
    assert proxy.index("direct_git_target_url") < proxy.index("masked_target_url")
    assert "FORKMESH_HOST" not in proxy

    initial_fetch = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "js_fetch_until_response")
    initial_fetch_source = ast.unparse(initial_fetch)
    assert "JsAbortController.new()" in initial_fetch_source
    assert "controller.abort.bind(controller)" in initial_fetch_source
    assert "js_clear_timeout(timer)" in initial_fetch_source
    assert "JsAbortSignal.timeout" not in initial_fetch_source

    advance = ast.unparse(next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_https_mirror_route_advance"))
    assert "clone_sticky" in advance
    assert "operation == 'git-info-refs'" in advance


def test_receive_pack_fails_closed_without_consuming_request_body():
    # Push is implemented through the signed masked proxy; an unsigned probe
    # (the common crawler case) is still answered before its body is read.
    push = _method_source("Default", "_git_push")
    assert "_https_mirror_push_proxy" in push
    assert "request.bytes" not in push
    assert "request.text" not in push
    proxy = _function_source("_https_mirror_push_proxy")
    # The refusal lives in one helper so it can also carry the Basic challenge
    # a stock git needs (issue #676); it must still answer before the body.
    assert proxy.index("x-forkmesh-pubkey") < proxy.index("request.bytes")
    assert proxy.index("_git_push_basic_challenge()") < proxy.index(
        "request.bytes")
    challenge = _function_source("_git_push_basic_challenge")
    assert "push_unauthorized" in challenge
    assert "status=401" in challenge
    assert "WWW-Authenticate" in challenge
    assert "push_too_large" in proxy
    assert "HTTPS_MIRROR_BODY_MAX_BYTES" in proxy
    # A pack above that cap is streamed, never buffered: the inbound body
    # stream is the upstream body and the capability carries the streamed
    # marker (or the digest a signing device stated), and that decision is
    # also made only after the authorization gate.
    assert "STREAMED_REQUEST_BODY_SHA256" in proxy
    assert "_https_mirror_request_body_stream" in proxy
    assert proxy.index("_git_push_basic_challenge()") < proxy.index(
        "_https_mirror_request_body_stream")
    assert "_https_mirror_push_send(" in proxy
    send = _function_source("_https_mirror_push_send")
    assert "HTTPS_MIRROR_PUSH_RESPONSE_TIMEOUT_SECONDS" in send


def test_batched_blobs_use_direct_https_gateway():
    route = _method_source("Default", "_route")
    gateway = (ROOT.parent / "tools" / "mirror_gateway.py").read_text(
        encoding="utf-8")
    runtime = ast.parse((ENTRY.parent / "app_runtime.py").read_text(
        encoding="utf-8"))
    read_actions = next(
        ast.literal_eval(node.value.args[0])
        for node in runtime.body
        if isinstance(node, ast.Assign)
        and any(getattr(t, "id", None) == "REPO_HOST_READ_ACTIONS"
                for t in node.targets))
    assert "blobs" in read_actions
    assert "action in REPO_HOST_READ_ACTIONS" in route
    assert "_https_mirror_proxy" in route
    assert "list(paths)[:60]" in gateway
