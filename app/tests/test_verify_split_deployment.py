import importlib.util
from pathlib import Path

import pytest


MODULE = Path(__file__).resolve().parents[1] / "tools/verify_split_deployment.py"
SPEC = importlib.util.spec_from_file_location("verify_split_deployment", MODULE)
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


def test_official_www_origins_handoff_both_world_paths(monkeypatch):
    monkeypatch.setattr(verifier, "WWW", "https://forkmesh.com")
    monkeypatch.setattr(verifier, "WORLD", "https://world.forkmesh.com")
    monkeypatch.delenv("DEPLOY_VERIFY_WWW_ALIAS_URL", raising=False)
    calls = []

    def fake_request(url, *, method="GET", headers=None, body=None):
        calls.append((url, method))
        path = "/" if url.endswith("/world") else "/world/"
        return 308, {"Location": "https://world.forkmesh.com" + path}, b""

    monkeypatch.setattr(verifier, "request", fake_request)

    for origin in verifier.www_origins():
        verifier.verify_world_handoff(origin)

    assert calls == [
        ("https://forkmesh.com/world", "HEAD"),
        ("https://forkmesh.com/world/", "HEAD"),
    ]


def test_main_checks_the_primary_and_alias_world_handoffs(monkeypatch):
    monkeypatch.setattr(verifier, "APP", "https://app.forkmesh.com")
    monkeypatch.setattr(verifier, "WWW", "https://forkmesh.com")
    monkeypatch.setattr(verifier, "WORLD", "https://world.forkmesh.com")
    monkeypatch.delenv("DEPLOY_VERIFY_WWW_ALIAS_URL", raising=False)
    handoffs = []
    blogs = []
    logins = []

    def fake_request(url, *, method="GET", headers=None, body=None):
        headers = headers or {}
        if method == "HEAD" and url in (
            "https://forkmesh.com/",
            "https://world.forkmesh.com/",
        ):
            worker = "www" if "forkmesh.com/" == url.removeprefix("https://") else "world"
            return 200, {"x-forkmesh-worker": worker}, b""
        if url == "https://app.forkmesh.com/api/version" and method == "GET":
            return 200, {"x-forkmesh-worker": "app"}, b'{"worker":"app","deployFingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'
        if url == "https://app.forkmesh.com/" and method == "GET":
            return 308, {
                "x-forkmesh-worker": "app", "location": "/dashboard",
            }, b""
        if url == "https://app.forkmesh.com/health" and method == "GET":
            return 200, {"x-forkmesh-worker": "app"}, b'{"ok":true}'
        if url == "https://app.forkmesh.com/api/mirrors/https" and method == "GET":
            return 200, {"x-forkmesh-worker": "app"}, (
                b'{"ok":true,"routerPublicKey":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}'
            )
        if url.removeprefix("https://app.forkmesh.com") in verifier.PUBLIC_SNAPSHOT_PATHS:
            if headers.get("authorization"):
                return 200, {}, b'{}'
            return 200, {
                "x-forkmesh-worker": "app",
                "x-forkmesh-public-snapshot": "fresh",
                "x-forkmesh-snapshot-fingerprint": "a" * 64,
            }, b'{}'
        if url == "https://app.forkmesh.com/api/status?fresh=post-deploy-verification":
            return 200, {}, b'{}'
        if url == "https://forkmesh.com/api/version" and method == "POST":
            return 200, {}, b'{"worker":"app"}'
        if method == "OPTIONS":
            response_headers = {}
            if headers.get("origin") == "https://world.forkmesh.com":
                response_headers["access-control-allow-origin"] = headers["origin"]
            return 204, response_headers, b""
        if url.endswith("/api/world/ws"):
            return 426, {}, b""
        raise AssertionError((url, method, headers, body))

    monkeypatch.setattr(verifier, "request", fake_request)
    monkeypatch.setattr(
        verifier, "verify_world_handoff", lambda origin: handoffs.append(origin)
    )
    monkeypatch.setattr(
        verifier, "verify_blog", lambda origin: blogs.append(origin)
    )
    monkeypatch.setattr(verifier, "verify_login", lambda: logins.append(True))

    assert verifier.main() == 0
    # Next serves one apex; there is no www alias to hand off separately.
    assert handoffs == ["https://forkmesh.com"]
    assert blogs == ["https://forkmesh.com"]
    assert logins == [True]


def test_main_rejects_legacy_edge_marker_even_with_app_and_fresh_headers(
    monkeypatch,
):
    monkeypatch.setattr(verifier, "APP", "https://app.forkmesh.com")

    def fake_request(url, *, method="GET", headers=None, body=None):
        if url.endswith("/api/version") and method == "GET":
            return 200, {
                "x-forkmesh-worker": "app",
                "x-forkmesh-edge-control": "active",
            }, b'{"worker":"app","deployFingerprint":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'
        raise AssertionError("verification should stop at the ownership gate")

    monkeypatch.setattr(verifier, "request", fake_request)
    with pytest.raises(RuntimeError, match="exclusive consolidated ownership"):
        verifier.main()


def test_blog_verifier_checks_index_rss_and_release_article(monkeypatch):
    calls = []

    def fake_request(url, **_kwargs):
        calls.append(url)
        if url.endswith("/blog"):
            return 200, {}, (
                b'<title>Blog \xc2\xb7 ForkMesh</title>'
                b'<a href="/blog/rss.xml">RSS</a>'
                b'<a href="/blog/one-app-one-mesh/">release</a>'
            )
        if url.endswith("/blog/rss.xml"):
            return 200, {"content-type": "application/rss+xml"}, b"<rss version='2.0'>"
        if url.endswith("/blog/one-app-one-mesh/"):
            return 200, {}, b"<h1>One app, one mesh</h1>"
        raise AssertionError(url)

    monkeypatch.setattr(verifier, "request", fake_request)
    verifier.verify_blog("https://forkmesh.com")
    assert calls == [
        "https://forkmesh.com/blog",
        "https://forkmesh.com/blog/rss.xml",
        "https://forkmesh.com/blog/one-app-one-mesh/",
    ]


def test_login_verifier_checks_shell_asset_canonicalization_and_api(monkeypatch):
    calls = []

    def fake_request(url, *, method="GET", headers=None, body=None):
        calls.append((url, method, body))
        if "/login?" in url:
            return 200, {}, (
                b'<title>Log in \xc2\xb7 ForkMesh</title>'
                b'<h1 id="login-title">Welcome back</h1>'
                b'<script src="/login.js?v=90129b56fcad" defer></script>'
            )
        if "/login.js?" in url:
            return 200, {"content-type": "text/javascript"}, b""
        if "/login/?" in url:
            return 307, {
                "location": "/login?next=%2Fdashboard%3Ftab%3Drepos",
            }, b""
        if url.endswith("/login.html"):
            return 404, {}, b""
        if url.endswith("/api/accounts/login"):
            assert method == "POST" and body == b"{"
            return 400, {}, b'{}'
        raise AssertionError((url, method))

    monkeypatch.setattr(verifier, "APP", "https://app.forkmesh.com")
    monkeypatch.setattr(verifier, "request", fake_request)
    verifier.verify_login()
    assert len(calls) == 5


def test_world_handoff_rejects_cohosted_world_content(monkeypatch):
    monkeypatch.setattr(
        verifier,
        "request",
        lambda *args, **kwargs: (
            200,
            {"x-forkmesh-worker": "world"},
            b"co-hosted world",
        ),
    )

    with pytest.raises(RuntimeError, match="returned 200; expected a redirect"):
        verifier.verify_world_handoff("https://forkmesh.com")


def test_world_handoff_requires_the_configured_world_origin(monkeypatch):
    monkeypatch.setattr(verifier, "WORLD", "https://world.mesh.example")
    monkeypatch.setattr(
        verifier,
        "request",
        lambda *args, **kwargs: (
            308,
            {"location": "https://app.mesh.example/world/"},
            b"",
        ),
    )

    with pytest.raises(RuntimeError, match="outside the configured World origin"):
        verifier.verify_world_handoff("https://mesh.example")


def test_custom_self_host_does_not_assume_a_www_alias(monkeypatch):
    monkeypatch.setattr(verifier, "WWW", "https://forkmesh.mesh.example")
    monkeypatch.delenv("DEPLOY_VERIFY_WWW_ALIAS_URL", raising=False)

    assert verifier.www_origins() == ["https://forkmesh.mesh.example"]

    monkeypatch.setenv(
        "DEPLOY_VERIFY_WWW_ALIAS_URL", "https://www.forkmesh.mesh.example/"
    )
    assert verifier.www_origins() == [
        "https://forkmesh.mesh.example",
        "https://www.forkmesh.mesh.example",
    ]
