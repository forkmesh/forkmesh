"""Independent mirror dashboard: same-codebase site served from git blobs.

The mirror serves the byte-identical dashboard the relay Worker serves, sourced
from the build outputs already present in the mirrored assets repository, plus
the read-only /api subset the logged-out dashboard needs. These tests pin asset
routing, the repo-page router, the CSP relaxation, the /api/repo read alias, and
the catalog/mirrors/reachability endpoints.
"""

import base64
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "app" / "src"))

import mirror_gateway as gateway  # noqa: E402
import mirror_replica  # noqa: E402
import portable_sessions  # noqa: E402
import public_snapshots  # noqa: E402


PUBLIC_KEY = base64.urlsafe_b64encode(b"N" * 32).decode().rstrip("=")
ROUTER_KEY = base64.urlsafe_b64encode(b"R" * 32).decode().rstrip("=")
SIGNATURE = base64.urlsafe_b64encode(b"S" * 64).decode().rstrip("=")
ORIGIN = "https://mirror33.forkmesh.example"
NOW = 2_000_000_000_000
NETWORK_PAYLOAD = {
    "ok": True,
    "stats": {"ok": True},
    "leaderboards": {"ok": True},
    "history": {"ok": True},
}
REPLICATED_MIRRORS_PAYLOAD = {
    "ok": True,
    "owner": "forkmesh",
    "repo": "forkmesh",
    "groupKey": "forkmesh/forkmesh",
    "generatedAt": NOW,
    "summary": {
        "mirrors": 3,
        "online": 3,
        "cloneAvailable": 3,
        "dataHostedBytes": 1500,
        "longestHostedSince": NOW - 60_000,
        "freshestSync": NOW,
    },
    "mirrors": [{
        "node": "mirror35",
        "status": "online",
        "endpointFresh": True,
        "endpointHealthy": True,
        "checkedAt": NOW,
    }],
    "sourceOfTruth": {
        "owner": "forkmesh",
        "repo": "forkmesh",
        "kind": "working-copy",
        "isSourceOfTruth": True,
    },
}
REPLICATED_ABOUT_PAYLOAD = {
    "ok": True,
    "description": "Portable About card",
    "website": "https://forkmesh.com",
    "logoUrl": (
        "/api/repo/forkmesh/forkmesh/raw?path=logo.png&ref=" + "1" * 40
    ),
    "logo": {},
    "bannerUrl": (
        "/api/repo/forkmesh/forkmesh/raw?path=.forkmesh/banner.png&ref="
        + "1" * 40
    ),
    "defaultLogoUrl": "/assets/fediverse-avatar.png",
    "fediverse": {
        "enabled": True,
        "handle": "@forkmesh.forkmesh@forkmesh.com",
        "actorUrl": "https://forkmesh.com/ap/repos/forkmesh/forkmesh",
        "followers": 4,
        "followersList": [],
        "settings": {
            "federate": True,
            "broadcastEvents": True,
            "acceptComments": True,
        },
    },
}


def _run(command, cwd):
    return subprocess.run(
        command, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


DASHBOARD_JS = b"// forkmesh dashboard bundle\nconsole.log('mirror');\n"
API_CLIENT_JS = b"window.ForkMeshAPI = {};\n"
MEMBERSHIP_PROMPT_JS = b"window.ForkMeshMembershipPrompt = {};\n"
TAILWIND_CSS = b":root{--x:1}\n"
REPO_HTML = (
    b"<!doctype html><html><head><title>Repo</title>"
    b'<script src="/dashboard.js?v=${version}"></script></head>'
    b"<body>repo</body></html>"
)
INDEX_HTML = b"<!doctype html><html><head><title>Dashboard</title></head><body>home</body></html>"
REPOS_HTML = b"<!doctype html><html><body>repos</body></html>"
NOT_FOUND_HTML = b"<!doctype html><html><body>404</body></html>"
FAVICON = b"\x00\x00\x01\x00"
FILE_ICON = b"<svg></svg>"


def _make_assets_repository(tmp_path, *, include_banner=False):
    bare = tmp_path / "assets.git"
    source = tmp_path / "assets-source"
    _run(["git", "init", "-b", "main", str(source)], tmp_path)
    _run(["git", "config", "user.email", "a@example.test"], source)
    _run(["git", "config", "user.name", "Alice"], source)
    files = {
        "app/public/dashboard.js": DASHBOARD_JS,
        "app/public/membership-prompt.js": MEMBERSHIP_PROMPT_JS,
        "www/public/api-client.js": API_CLIENT_JS,
        "app/public/dashboard/tailwind.css": TAILWIND_CSS,
        "app/public/dashboard/repo.html": REPO_HTML,
        "app/public/dashboard/index.html": INDEX_HTML,
        "app/public/dashboard/repos/index.html": REPOS_HTML,
        "www/public/404.html": NOT_FOUND_HTML,
        "www/public/favicon/favicon.ico": FAVICON,
        "app/public/assets/file-icons/python.svg": FILE_ICON,
        "logo.png": b"\x89PNG\r\n\x1a\nrepository-logo",
        ".forkmesh/projects/1/project-1.json": (
            b'{"number":1,"title":"Mirror independence","status":"open",'
            b'"issues":[]}'
        ),
        "README.md": b"# assets\n",
    }
    if include_banner:
        files[".forkmesh/banner.png"] = (
            b"\x89PNG\r\n\x1a\nrepository-banner"
        )
    for relative, content in files.items():
        target = source / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    _run(["git", "add", "."], source)
    _run(["git", "commit", "-m", "Dashboard build outputs"], source)
    _run(["git", "clone", "--bare", str(source), str(bare)], tmp_path)
    _run(["git", "symbolic-ref", "HEAD", "refs/heads/main"], bare)
    return bare


def _signed_manifest():
    manifest = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-endpoint",
        "generatedAt": "2026-08-19T10:00:00Z",
        "node": {"name": "mirror33", "publicKey": PUBLIC_KEY},
        "endpoint": {
            "origin": ORIGIN,
            "healthUrl": ORIGIN + "/health",
            "manifestUrl": ORIGIN + "/forkmesh-mirror.json",
            "repositoryUrlTemplate": ORIGIN + "/{owner}/{repository}",
            "transport": "direct-https",
            "mainProxyMode": "direct",
        },
        "dns": {
            "recordName": "mirror33.forkmesh.example",
            "proxied": False,
            "recordType": "A/AAAA",
        },
        "storage": {
            "repositoryBytesInD1": False,
            "repositoryByteOwner": "independent-mirror-host",
            "d1Purpose": ["service-discovery"],
        },
    }
    payload = gateway._canonical_json(manifest).encode()
    manifest["signature"] = {
        "algorithm": "Ed25519",
        "encoding": "base64url-no-padding",
        "canonicalization": "forkmesh-json-sort-v1",
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "value": SIGNATURE,
    }
    return manifest


def _write_config(tmp_path, bare, *, dashboard=True, ssh_origin=True):
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(json.dumps(_signed_manifest()), encoding="utf-8")
    config = {
        "schemaVersion": 1,
        "node": {"name": "mirror33", "publicKey": PUBLIC_KEY},
        "routerPublicKey": ROUTER_KEY,
        "publicOrigin": ORIGIN,
        "listen": {"host": "127.0.0.1", "port": 8790},
        "manifestPath": str(manifest_path),
        "requestVerifierCommand": ["verify-capability"],
        "healthSignerCommand": ["sign-health"],
        "repositories": [
            {
                "owner": "forkmesh",
                "name": "forkmesh",
                "visibility": "public",
                "enabled": True,
                "gitDir": str(bare),
                "integrity": {"expectedRefsSha256": gateway.refs_sha256(bare)},
            }
        ],
    }
    if dashboard:
        dashboard_config = {
            "enabled": True,
            "assetsRepository": "forkmesh/forkmesh",
        }
        if ssh_origin:
            dashboard_config["sshOrigin"] = "ssh://git@ssh.forkmesh.example"
        config["dashboard"] = dashboard_config
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    return config_path


class _Verifier:
    def verify(self, message, signature):
        return signature == SIGNATURE


class _Signer:
    def sign(self, message):
        return SIGNATURE


@pytest.fixture
def dashboard_app(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config = gateway.load_config(_write_config(tmp_path, bare))
    app = gateway.GatewayApplication(
        config,
        verifier=_Verifier(),
        health_signer=_Signer(),
        clock_ms=lambda: NOW,
        log=lambda _entry: None,
    )
    yield app
    app.close()


def _get(app, path, *, method="GET", headers=None):
    return app.dispatch(method, path, headers or {}, b"")


def _stamped_repo_html(app):
    repository = app.repositories[("forkmesh", "forkmesh")]
    commit = _run(["git", "rev-parse", "HEAD"], repository.git_dir)
    return REPO_HTML.replace(b"${version}", commit[:12].encode())


def test_dashboard_pages_carry_a_real_asset_version(dashboard_app):
    page = _get(dashboard_app, "/forkmesh/forkmesh")
    assert page.status == 200
    assert b"${version}" not in page.body
    assert page.body == _stamped_repo_html(dashboard_app)


def _built_site(root, marker):
    files = {
        "dashboard.js": b"// built " + marker + b"\n",
        "user-status.js": b"// status " + marker + b"\n",
        "api-client.js": API_CLIENT_JS,
        "dashboard/tailwind.css": TAILWIND_CSS,
        "dashboard/repo.html": (
            b'<!doctype html><script src="/dashboard.js?v=' + marker
            + b'"></script>'
        ),
        "dashboard/index.html": b"<!doctype html>home " + marker,
        "404.html": b"<!doctype html>missing " + marker,
        "favicon/favicon.ico": FAVICON,
        "assets/file-icons/python.svg": FILE_ICON,
        "_headers": b"/*\n",
    }
    for relative, content in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)


def _built_site_app(tmp_path):
    bare = _make_assets_repository(tmp_path)
    releases = tmp_path / "releases"
    _built_site(releases / "one" / "app" / "dist", b"one")
    _built_site(releases / "two" / "app" / "dist", b"two")
    current = tmp_path / "current"
    current.symlink_to(releases / "one")
    config_path = _write_config(tmp_path, bare)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["dashboard"]["assetsRoot"] = str(current / "app" / "dist")
    config_path.write_text(json.dumps(config), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=_Verifier(),
        health_signer=_Signer(),
        clock_ms=lambda: NOW,
        log=lambda _entry: None,
    )
    return app, current, releases


def test_dashboard_assets_root_serves_the_deployed_build(tmp_path):
    app, current, releases = _built_site_app(tmp_path)
    try:
        assert _get(app, "/dashboard.js?v=one").body == b"// built one\n"
        assert _get(app, "/user-status.js").body == b"// status one\n"
        assert _get(app, "/favicon.ico").body == FAVICON
        assert _get(app, "/assets/file-icons/python.svg").body == FILE_ICON
        page = _get(app, "/forkmesh/forkmesh")
        assert page.status == 200
        assert b"/dashboard.js?v=one" in page.body
        assert _get(app, "/dashboard").body == b"<!doctype html>home one"
        assert _get(app, "/_headers").status == 404
        missing = _get(app, "/nothing-here.js")
        assert missing.status == 404 and missing.body == b"<!doctype html>missing one"
        etag = _get(app, "/dashboard.js").headers["ETag"]

        current.unlink()
        current.symlink_to(releases / "two")
        assert _get(app, "/dashboard.js").body == b"// built two\n"
        assert _get(app, "/dashboard.js").headers["ETag"] != etag
        assert b"/dashboard.js?v=two" in _get(app, "/forkmesh/forkmesh").body
    finally:
        app.close()


def test_dashboard_assets_root_refuses_escapes(tmp_path):
    app, current, releases = _built_site_app(tmp_path)
    try:
        (tmp_path / "secret.js").write_bytes(b"secret")
        (releases / "one" / "app" / "dist" / "leak.js").symlink_to(
            tmp_path / "secret.js")
        for path in ("/leak.js", "/assets/../../secret.js",
                     "/assets/..%2f..%2fsecret.js"):
            response = _get(app, path)
            assert response.status == 404, path
            assert b"secret" not in response.body
    finally:
        app.close()


def test_dashboard_assets_root_must_be_absolute(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config_path = _write_config(tmp_path, bare)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    for bad in ("relative/dist", "/opt/../etc", "", 7):
        config["dashboard"]["assetsRoot"] = bad
        config_path.write_text(json.dumps(config), encoding="utf-8")
        with pytest.raises(gateway.GatewayError, match="assetsRoot"):
            gateway.load_config(config_path)


# ---------------------------------------------------------------------------
# static assets
# ---------------------------------------------------------------------------


def test_dashboard_serves_bundle_and_assets_from_git_blobs(dashboard_app):
    bundle = _get(dashboard_app, "/dashboard.js")
    assert bundle.status == 200
    assert bundle.body == DASHBOARD_JS
    assert bundle.content_type == "text/javascript; charset=utf-8"
    assert bundle.headers["Content-Security-Policy"].startswith("default-src 'self'")
    assert bundle.headers["Cache-Control"] == "public, max-age=300"
    assert bundle.headers["ETag"]

    api_client = _get(dashboard_app, "/api-client.js")
    assert api_client.status == 200 and api_client.body == API_CLIENT_JS

    membership = _get(
        dashboard_app, "/membership-prompt.js?v=content-hash"
    )
    assert membership.status == 200
    assert membership.body == MEMBERSHIP_PROMPT_JS

    css = _get(dashboard_app, "/dashboard/tailwind.css")
    assert css.status == 200 and css.content_type == "text/css; charset=utf-8"

    favicon = _get(dashboard_app, "/favicon/favicon.ico")
    assert favicon.status == 200 and favicon.body == FAVICON

    icon = _get(dashboard_app, "/assets/file-icons/python.svg")
    assert icon.status == 200 and icon.content_type == "image/svg+xml"

    analytics = _get(dashboard_app, "/analytics.js")
    assert analytics.status == 200 and b"disabled" in analytics.body


def test_dashboard_asset_etag_supports_conditional_requests(dashboard_app):
    first = _get(dashboard_app, "/dashboard.js")
    etag = first.headers["ETag"]
    second = _get(
        dashboard_app, "/dashboard.js", headers={"If-None-Match": etag}
    )
    assert second.status == 304
    assert second.body == b""


def test_dashboard_asset_hot_read_avoids_repeat_git(dashboard_app, monkeypatch):
    first = _get(dashboard_app, "/dashboard.js")
    assert first.status == 200

    def unexpected_git(*_args, **_kwargs):
        raise AssertionError("hot dashboard asset read reached Git")

    monkeypatch.setattr(gateway, "_run_git", unexpected_git)
    second = _get(dashboard_app, "/dashboard.js")
    assert second.status == 200
    assert second.body == first.body
    diagnostics = dashboard_app.cache_diagnostics()["dashboardAssets"]
    assert diagnostics["hits"] == 1
    assert diagnostics["backendReads"] == 1


def test_dashboard_startup_prewarm_loads_shell_and_catalog(
    dashboard_app, monkeypatch
):
    dashboard_app.prewarm_memory_caches()
    diagnostics = dashboard_app.cache_diagnostics()
    assert diagnostics["dashboardAssets"]["entries"] >= 5
    assert diagnostics["repositoryCatalog"]["entries"] == 1

    def unexpected_git(*_args, **_kwargs):
        raise AssertionError("prewarmed dashboard read reached Git")

    monkeypatch.setattr(gateway, "_run_git", unexpected_git)
    assert _get(dashboard_app, "/dashboard.js").body == DASHBOARD_JS
    assert _get(dashboard_app, "/api/repositories").status == 200


def test_dashboard_rejects_asset_path_traversal(dashboard_app):
    for path in (
        "/assets/../../../etc/passwd",
        "/assets/..%2f..%2fsecret",
        "/favicon/../../www/worker.js",
    ):
        response = _get(dashboard_app, path)
        assert response.status == 404


# ---------------------------------------------------------------------------
# HTML pages and the repo-page router
# ---------------------------------------------------------------------------


def test_dashboard_serves_pages_and_repo_router(dashboard_app):
    root = _get(dashboard_app, "/")
    assert root.status == 308 and root.headers["Location"] == "/dashboard"

    home = _get(dashboard_app, "/dashboard")
    assert home.status == 200 and home.body == INDEX_HTML
    assert home.headers["X-Frame-Options"] == "DENY"
    assert home.content_type == "text/html; charset=utf-8"

    repos = _get(dashboard_app, "/dashboard/repos")
    assert repos.status == 200 and repos.body == REPOS_HTML

    # Only a repository this independent mirror actually serves gets the repo
    # shell. Deep links stay available for that configured identity.
    for path in (
        "/forkmesh/forkmesh",
        "/forkmesh/forkmesh/tree/main",
    ):
        page = _get(dashboard_app, path)
        assert page.status == 200, path
        assert page.body == _stamped_repo_html(dashboard_app)

    # Stale node-owner aliases and arbitrary owner/repo shapes must not look
    # like live repositories. The canonical route for this mirror is the
    # configured forkmesh/forkmesh identity above.
    for path in (
        "/mirror33/forkmesh",
        "/mirror33/forkmesh/tree/main",
        "/alice/private-thing/issues/3",
    ):
        page = _get(dashboard_app, path)
        assert page.status == 404, path
        assert page.body == NOT_FOUND_HTML

    missing_page = _get(dashboard_app, "/dashboard/does-not-exist")
    assert missing_page.status == 404
    assert missing_page.body == NOT_FOUND_HTML


def test_dashboard_router_does_not_claim_reserved_paths(dashboard_app):
    # A reserved first segment is never treated as an owner: it 404s as a page
    # rather than rendering the repo shell.
    for path in ("/world/lobby", "/blog/post", "/docs/guide"):
        response = _get(dashboard_app, path)
        assert response.body != REPO_HTML
    # Single-segment unknown paths are not repo routes either.
    assert _get(dashboard_app, "/forkmesh").body != REPO_HTML


def test_login_and_signup_redirect_to_canonical_when_configured(tmp_path):
    bare = _make_assets_repository(tmp_path)
    manifest_path = tmp_path / "manifest.json"
    manifest_path.write_text(json.dumps(_signed_manifest()), encoding="utf-8")
    config = json.loads(
        _write_config(tmp_path, bare).read_text(encoding="utf-8")
    )
    config["dashboard"]["canonicalOrigin"] = "https://app.forkmesh.example"
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=_Verifier(),
        health_signer=_Signer(),
        clock_ms=lambda: NOW,
    )
    try:
        login = _get(app, "/login")
        assert login.status == 308
        assert login.headers["Location"] == (
            "https://app.forkmesh.example/login"
        )
    finally:
        app.close()


# ---------------------------------------------------------------------------
# /api subset
# ---------------------------------------------------------------------------


def test_api_repositories_projects_the_local_catalog(dashboard_app):
    response = _get(dashboard_app, "/api/repositories")
    assert response.status == 200
    payload = json.loads(response.body)
    assert payload["ok"] is True
    assert payload["sshGatewayConfigured"] is True
    repositories = payload["repositories"]
    assert len(repositories) == 1
    row = repositories[0]
    assert row["owner"] == "forkmesh"
    assert row["name"] == "forkmesh"
    assert row["visibility"] == "public"
    assert row["isPrivate"] is False
    assert row["cloneUrl"] == ORIGIN + "/forkmesh/forkmesh.git"
    assert row["mirrorUrl"] == ORIGIN + "/forkmesh/forkmesh.git"
    assert row["dashboardUrl"] == ORIGIN + "/forkmesh/forkmesh"
    assert row["source"] == "mirror-node"
    assert row["liveHost"] == "mirror33"
    assert row["sshUrl"] == "ssh://git@ssh.forkmesh.example/forkmesh/forkmesh.git"
    assert row["branch"] == "main"
    assert row["branchCount"] >= 1
    # Every projected field is a recognized public catalog key.

    assert set(row) <= public_snapshots.CATALOG_REPOSITORY_KEYS


def test_api_repository_catalog_hot_read_avoids_repeat_git(
    dashboard_app, monkeypatch
):
    first = _get(dashboard_app, "/api/repositories")
    assert first.status == 200

    def unexpected_git(*_args, **_kwargs):
        raise AssertionError("hot repository catalog read reached Git")

    monkeypatch.setattr(gateway, "_run_git", unexpected_git)
    second = _get(dashboard_app, "/api/repositories")
    assert second.status == 200
    assert second.body == first.body
    diagnostics = dashboard_app.cache_diagnostics()["repositoryCatalog"]
    assert diagnostics["hits"] == 1
    assert diagnostics["backendReads"] == 1


def test_api_version_and_sessions_endpoints(dashboard_app):
    version = _get(dashboard_app, "/api/version")
    assert version.status == 200
    assert json.loads(version.body)["version"].startswith("mirror/")
    sessions = _get(dashboard_app, "/api/accounts/sessions")
    assert sessions.status == 401


def test_signed_portable_session_stays_logged_in_on_independent_mirror(
    dashboard_app, monkeypatch,
):
    private_key = Ed25519PrivateKey.generate()
    public_key = base64.urlsafe_b64encode(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).decode().rstrip("=")
    config = replace(dashboard_app.config, router_public_key=public_key)
    dashboard_app.config = config
    dashboard_app.dashboard.config = config
    claim = portable_sessions.build_claim(
        account_id="a" * 64,
        account_name="alice",
        session_id="s" * 32,
        grant_id="g" * 24,
        issued_at=NOW,
        expires_at=NOW + 60_000,
    )
    encoded = portable_sessions.encode_claim(claim)
    signature = base64.urlsafe_b64encode(
        private_key.sign(portable_sessions.signing_message(encoded))
    ).decode().rstrip("=")
    token = portable_sessions.assemble_token(encoded, signature)
    headers = {"Cookie": "forkmesh_portable=" + token}
    real_parse = portable_sessions.parse_token
    verification_count = 0

    def counted_parse(*args, **kwargs):
        nonlocal verification_count
        verification_count += 1
        return real_parse(*args, **kwargs)

    monkeypatch.setattr(portable_sessions, "parse_token", counted_parse)

    sessions = _get(
        dashboard_app, "/api/accounts/sessions", headers=headers
    )
    assert sessions.status == 200
    payload = json.loads(sessions.body)
    assert payload["portable"] is True
    assert payload["controllerIndependent"] is True
    assert payload["account"]["nodeName"] == "alice"
    assert payload["sessions"][0]["expiresAt"] == NOW + 60_000
    assert _get(dashboard_app, "/login", headers=headers).headers[
        "Location"
    ] == "/dashboard"
    assert verification_count == 1
    diagnostics = dashboard_app.cache_diagnostics()["portableSessions"]
    assert diagnostics["hits"] == 1
    assert diagnostics["backendReads"] == 1

    forged = token[:-1] + ("A" if token[-1] != "A" else "B")
    denied = _get(
        dashboard_app,
        "/api/accounts/sessions",
        headers={"Cookie": "forkmesh_portable=" + forged},
    )
    assert denied.status == 401


def test_app_form_handoff_sets_shared_cookie_before_mirror_redirect(
    dashboard_app,
):
    private_key = Ed25519PrivateKey.generate()
    public_key = base64.urlsafe_b64encode(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).decode().rstrip("=")
    config = replace(
        dashboard_app.config,
        router_public_key=public_key,
        public_origin="https://mirror37.forkmesh.com",
        dashboard_canonical_origin="https://app.forkmesh.com",
    )
    dashboard_app.config = config
    dashboard_app.dashboard.config = config
    claim = portable_sessions.build_claim(
        account_id="a" * 64,
        account_name="alice",
        session_id="s" * 32,
        grant_id="g" * 24,
        issued_at=NOW,
        expires_at=NOW + 60_000,
    )
    encoded = portable_sessions.encode_claim(claim)
    signature = base64.urlsafe_b64encode(
        private_key.sign(portable_sessions.signing_message(encoded))
    ).decode().rstrip("=")
    token = portable_sessions.assemble_token(encoded, signature)
    next_path = "/forkmesh/forkmesh?theme=dark"
    body = urlencode({"grant": token, "next": next_path}).encode()
    response = dashboard_app.dispatch(
        "POST",
        "/api/accounts/session-handoff",
        {
            "Origin": "https://app.forkmesh.com",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        body,
    )
    assert response.status == 303
    assert response.headers["Location"] == next_path
    cookie = response.headers["Set-Cookie"]
    assert cookie.startswith("forkmesh_portable=" + token + "; Path=/")
    assert "; Domain=.forkmesh.com" in cookie
    assert "; HttpOnly; Secure; SameSite=Strict" in cookie
    assert token not in response.headers["Location"]
    assert response.headers["Cache-Control"].startswith("no-store")

    cookie_pair = cookie.split(";", 1)[0]
    sessions = _get(
        dashboard_app,
        "/api/accounts/sessions",
        headers={"Cookie": cookie_pair},
    )
    assert sessions.status == 200
    assert json.loads(sessions.body)["account"]["nodeName"] == "alice"

    denied = dashboard_app.dispatch(
        "POST",
        "/api/accounts/session-handoff",
        {
            "Origin": "https://attacker.example",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        body,
    )
    assert denied.status == 401


def test_dashboard_serves_last_good_public_projection_offline(
    tmp_path, monkeypatch,
):
    bare = _make_assets_repository(tmp_path, include_banner=True)
    replica_path = tmp_path / "dashboard-replica.json"
    mirror_replica.refresh_bundle(
        replica_path,
        "https://app.forkmesh.example",
        fetch=lambda url: {
            "/api/network/overview": (200, NETWORK_PAYLOAD),
            "/api/repo/forkmesh/forkmesh/mirrors": (
                200, REPLICATED_MIRRORS_PAYLOAD
            ),
            "/api/repo/forkmesh/forkmesh/star": (
                200, {"ok": True, "count": 6, "starred": False}
            ),
            "/api/repo/forkmesh/forkmesh/forks": (
                200, {"ok": True, "count": 2, "forks": []}
            ),
            "/api/repo/forkmesh/forkmesh/about": (
                200, REPLICATED_ABOUT_PAYLOAD
            ),
        }.get(
            url.removeprefix("https://app.forkmesh.example"),
            (404, None)
            if url.endswith(mirror_replica.REPLICA_SOURCE_PATH)
            else (503, None),
        ),
        now_ms=lambda: NOW,
    )
    config = json.loads(
        _write_config(tmp_path, bare).read_text(encoding="utf-8")
    )
    config["dashboard"].update({
        "canonicalOrigin": "https://app.forkmesh.example",
        "replicaPath": str(replica_path),
    })
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    app = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=_Verifier(),
        health_signer=_Signer(),
        clock_ms=lambda: NOW,
    )
    try:
        response = _get(app, "/api/network/overview")
        assert response.status == 200
        assert json.loads(response.body) == NETWORK_PAYLOAD
        assert response.headers["X-ForkMesh-Replica"] == "public-projection"
        assert response.headers["X-ForkMesh-Replica-Fetched-At"] == str(NOW)
        # Local Git state remains authoritative over a cached catalog row.
        catalog = _get(app, "/api/repositories")
        assert json.loads(catalog.body)["repositories"][0]["liveHost"] == "mirror33"

        # Controller-maintained public counts retain their last validated
        # values while the mirror remains otherwise independent.
        star = _get(app, "/api/repo/forkmesh/forkmesh/star")
        assert json.loads(star.body) == {
            "ok": True, "count": 6, "starred": False
        }
        forks = _get(app, "/api/repo/forkmesh/forkmesh/forks")
        assert json.loads(forks.body)["count"] == 2
        about = _get(app, "/api/repo/forkmesh/forkmesh/about")
        about_payload = json.loads(about.body)
        assert about_payload["fediverse"]["followers"] == 4
        assert "ref=" + "1" * 40 not in about_payload["bannerUrl"]
        assert "ref=" + "1" * 40 not in about_payload["logoUrl"]
        assert _get(app, about_payload["bannerUrl"]).status == 200
        assert _get(app, about_payload["logoUrl"]).status == 200
        logo = _get(app, "/api/repo/forkmesh/forkmesh/logo")
        assert logo.status == 200
        assert json.loads(logo.body)["logo"]["dataUrl"].startswith(
            "/api/repo/forkmesh/forkmesh/raw?"
        )
        mirrors = _get(app, "/api/repo/forkmesh/forkmesh/mirrors")
        assert json.loads(mirrors.body)["summary"]["mirrors"] == 3
        for response in (star, forks, about, mirrors):
            assert response.headers["X-ForkMesh-Replica"] == (
                "public-projection"
            )

        def unexpected_git(*_args, **_kwargs):
            raise AssertionError("hot repository media read reached Git")

        monkeypatch.setattr(gateway, "_run_git", unexpected_git)
        monkeypatch.setattr(gateway, "_run_git_status", unexpected_git)
        assert _get(
            app, "/api/repo/forkmesh/forkmesh/logo"
        ).status == 200
        assert _get(
            app, "/api/repo/forkmesh/forkmesh/about"
        ).status == 200
    finally:
        app.close()


def test_api_repo_read_alias_proxies_git_operations(dashboard_app):
    tree = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/tree",
        headers={"Origin": "https://app.forkmesh.com"},
    )
    assert tree.status == 200
    assert tree.headers["X-ForkMesh-Served-By"] == "mirror33"
    assert tree.headers["Access-Control-Allow-Origin"] == "*"
    assert "X-ForkMesh-Served-By" in tree.headers[
        "Access-Control-Expose-Headers"
    ]
    payload = json.loads(tree.body)
    assert payload["ok"] is True
    names = {entry["name"] for entry in payload["entries"]}
    assert "README.md" in names

    for operation in ("about", "mirrors"):
        public_projection = _get(
            dashboard_app,
            "/api/repo/forkmesh/forkmesh/" + operation,
            headers={"Origin": "https://app.forkmesh.com"},
        )
        # A first-boot node may not have received its controller projection
        # yet (About is then 404); either response must still be browser-readable
        # so the direct-reader can try the next healthy mirror.
        assert public_projection.status in (200, 404)
        assert public_projection.headers["Access-Control-Allow-Origin"] == "*"

    blob = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/blob?path=README.md",
    )
    assert blob.status == 200
    blob_payload = json.loads(blob.body)
    assert blob_payload["ok"] is True
    assert "assets" in blob_payload["content"]

    branches = _get(dashboard_app, "/api/repo/forkmesh/forkmesh/branches")
    assert branches.status == 200
    assert json.loads(branches.body)["ok"] is True

    # Projects are Git-authoritative too: a direct mirror must expose the
    # committed folder and project document without a controller round-trip.
    projects = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/tree?path=.forkmesh/projects",
    )
    assert projects.status == 200
    assert [
        entry["name"] for entry in json.loads(projects.body)["entries"]
    ] == ["1"]
    project = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/blob?path="
        ".forkmesh/projects/1/project-1.json",
    )
    assert project.status == 200
    assert "Mirror independence" in json.loads(project.body)["content"]


def test_direct_collaboration_preflight_is_credentialed_and_origin_bound(
    dashboard_app,
):
    assert callable(gateway.MirrorGatewayHandler.do_OPTIONS)
    config = replace(
        dashboard_app.config,
        dashboard_canonical_origin="https://app.forkmesh.example",
        collaboration_control_url=(
            "http://127.0.0.1:8791/v1/control/collaboration"
        ),
    )
    dashboard_app.config = config
    dashboard_app.dashboard.config = config
    headers = {
        "Origin": "https://app.forkmesh.example",
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "content-type, accept",
    }
    response = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/issues",
        method="OPTIONS",
        headers=headers,
    )
    assert response.status == 204
    assert response.headers["Access-Control-Allow-Origin"] == headers["Origin"]
    assert response.headers["Access-Control-Allow-Credentials"] == "true"
    denied = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/issues",
        method="OPTIONS",
        headers={**headers, "Origin": "https://attacker.example"},
    )
    assert denied.status == 404


def test_signed_issue_open_is_committed_through_local_daemon(
    dashboard_app, monkeypatch,
):
    config = replace(
        dashboard_app.config,
        dashboard_canonical_origin="https://app.forkmesh.example",
        collaboration_control_url=(
            "http://127.0.0.1:8791/v1/control/collaboration"
        ),
    )
    dashboard_app.config = config
    dashboard_app.dashboard.config = config
    private_key = Ed25519PrivateKey.generate()
    author = base64.urlsafe_b64encode(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).decode().rstrip("=")
    timestamp = NOW
    title = "Independent issue"
    body = "Accepted directly by a mirror"
    content = title + "\0" + body + "\0"
    canonical = (
        "forkmesh-issue-event-v1\nopen\n0\n" + author + "\n"
        + str(timestamp) + "\n"
        + hashlib.sha256(content.encode()).hexdigest()
    ).encode()
    signature = base64.urlsafe_b64encode(
        private_key.sign(canonical)
    ).decode().rstrip("=")
    event = {
        "type": "open", "id": "open-direct", "title": title,
        "body": body, "attachments": [], "author": author,
        "authorName": "untrusted", "ts": timestamp, "sig": signature,
    }
    forwarded = {}

    class LocalResponse:
        status = 201

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def read(self, _maximum):
            return json.dumps({
                "ok": True, "number": 44, "durable": True,
                "controllerIndependent": True,
            }).encode()

    def local_open(request, timeout):
        assert timeout == 20
        forwarded.update(json.loads(request.data))
        return LocalResponse()

    monkeypatch.setattr(gateway, "_open_loopback_control", local_open)
    response = dashboard_app.dispatch(
        "POST",
        "/api/repo/forkmesh/forkmesh/issues",
        {
            "Origin": "https://app.forkmesh.example",
            "Content-Type": "application/json",
        },
        json.dumps({
            "owner": "forkmesh", "repo": "forkmesh", "number": 0,
            "titleIfNew": title, "event": event,
        }).encode(),
    )
    assert response.status == 201
    assert json.loads(response.body)["number"] == 44
    assert response.headers["X-ForkMesh-Served-By"] == "mirror33"
    assert response.headers["Access-Control-Allow-Credentials"] == "true"
    assert forwarded["kind"] == "issues"
    assert forwarded["item"]["event"]["authorName"] == ""
    assert forwarded["item"]["event"]["sig"] == signature

    event["sig"] = signature[:-1] + ("A" if signature[-1] != "A" else "B")
    denied = dashboard_app.dispatch(
        "POST", "/api/repo/forkmesh/forkmesh/issues",
        {"Content-Type": "application/json"},
        json.dumps({"number": 0, "event": event}).encode(),
    )
    assert denied.status == 401


def test_direct_pull_and_discussion_signatures_match_browser_wire_format():
    private_key = Ed25519PrivateKey.generate()
    author = base64.urlsafe_b64encode(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).decode().rstrip("=")

    pull = {
        "title": "Independent pull",
        "base": "main",
        "head": "feature",
        "patch": "diff --git a/a b/a\n",
        "commits": "From 0123456789abcdef Mon Sep 17 00:00:00 2001\n",
        "author": author,
        "ts": NOW,
    }
    pull_content = "\0".join(
        str(pull[key]) for key in ("title", "base", "head", "patch", "commits")
    )
    pull_message = (
        "forkmesh-pull-event-v1\n" + author + "\n" + str(NOW) + "\n"
        + hashlib.sha256(pull_content.encode()).hexdigest()
    ).encode()
    pull["sig"] = base64.urlsafe_b64encode(
        private_key.sign(pull_message)
    ).decode().rstrip("=")
    assert gateway._verify_dashboard_pull(pull) is True

    discussion = {
        "type": "open",
        "title": "Independent discussion",
        "body": "No controller required",
        "category": "Ideas",
        "author": author,
        "ts": NOW,
    }
    discussion_content = "\0".join([
        discussion["title"], discussion["body"], discussion["category"],
    ])
    discussion_message = (
        "forkmesh-discussion-event-v1\nopen\n0\n" + author + "\n"
        + str(NOW) + "\n"
        + hashlib.sha256(discussion_content.encode()).hexdigest()
    ).encode()
    discussion["sig"] = base64.urlsafe_b64encode(
        private_key.sign(discussion_message)
    ).decode().rstrip("=")
    assert gateway._verify_dashboard_discussion(0, discussion) is True

    answer = {"type": "answer", "target": "reply-7", "author": author,
              "ts": NOW}
    answer_message = (
        "forkmesh-discussion-event-v1\nanswer\n3\n" + author + "\n"
        + str(NOW) + "\n" + hashlib.sha256(b"reply-7").hexdigest()
    ).encode()
    answer["sig"] = base64.urlsafe_b64encode(
        private_key.sign(answer_message)
    ).decode().rstrip("=")
    assert gateway._verify_dashboard_discussion(3, answer) is True
    assert gateway._verify_dashboard_discussion(
        3, dict(answer, target="reply-8")) is False
    assert gateway._verify_dashboard_discussion(0, answer) is False


def test_direct_discussion_answers_flag_only_the_owners_session():
    source = Path(gateway.__file__).read_text(encoding="utf-8")
    flag = source.index('item["meta"] = {"ownerApproved": True}')
    guard = source[flag - 260:flag]
    assert 'str(event.get("type") or "") == "answer"' in guard
    assert "actor == owner" in guard and "trusted_browser_origin" in guard


def test_gateway_event_signatures_bind_the_repository(monkeypatch):
    private_key = Ed25519PrivateKey.generate()
    author = base64.urlsafe_b64encode(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).decode().rstrip("=")

    def signed(message):
        return base64.urlsafe_b64encode(
            private_key.sign(message.encode())
        ).decode().rstrip("=")

    event = {"type": "comment", "body": "hello", "author": author, "ts": NOW}
    body = (
        "comment\n4\n" + author + "\n" + str(NOW) + "\n"
        + hashlib.sha256(b"hello").hexdigest()
    )
    v2 = dict(event, sig=signed(
        "forkmesh-pull-comment-v2\nalice/project\n" + body))
    assert gateway._verify_dashboard_pull_event(4, v2, "Alice", "Project")
    assert not gateway._verify_dashboard_pull_event(4, v2, "mallory", "other")

    v1 = dict(event, sig=signed("forkmesh-pull-comment-v1\n" + body))
    assert gateway._verify_dashboard_pull_event(4, v1, "mallory", "other")
    monkeypatch.setattr(gateway, "EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS", 0)
    assert not gateway._verify_dashboard_pull_event(4, v1, "mallory", "other")
    assert gateway._verify_dashboard_pull_event(4, v2, "alice", "project")


def test_api_repo_read_alias_drops_catalog_view_token_params(dashboard_app):
    # fmv/viewer/ts/sig are appended by the front-end; the strict gateway query
    # allowlist would reject them, so the alias must drop them first.
    response = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/tree?path=&ref=main&fmv=abc&viewer=x&ts=1&sig=y",
    )
    assert response.status == 200
    assert json.loads(response.body)["ok"] is True


def test_api_repo_read_alias_refuses_write_and_unknown_operations(dashboard_app):
    for path in (
        "/api/repo/forkmesh/forkmesh/merge-pull",
        "/api/repo/forkmesh/forkmesh/git-upload-pack",
        "/api/repo/forkmesh/forkmesh/nonsense",
    ):
        assert _get(dashboard_app, path).status == 404
    assert _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/tree",
        method="POST",
    ).status == 404


def test_api_repo_mirrors_reports_this_node(dashboard_app):
    response = _get(dashboard_app, "/api/repo/forkmesh/forkmesh/mirrors")
    assert response.status == 200
    payload = json.loads(response.body)
    assert payload["ok"] is True
    assert payload["summary"]["mirrors"] == 1
    assert payload["summary"]["online"] == 1
    mirror = payload["mirrors"][0]
    assert mirror["node"] == "mirror33"
    assert mirror["mirrorUrl"] == ORIGIN + "/forkmesh/forkmesh.git"
    assert mirror["dashboardUrl"] == ORIGIN + "/forkmesh/forkmesh"
    assert mirror["cloneAvailable"] is True
    assert mirror["endpoint"] == ORIGIN


    assert set(payload) <= public_snapshots.MIRROR_RESPONSE_KEYS
    assert set(mirror) <= public_snapshots.MIRROR_ITEM_KEYS
    assert set(payload["summary"]) <= public_snapshots.MIRROR_SUMMARY_KEYS


def test_api_repo_reachability_probes_this_node(dashboard_app):
    response = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/mirrors/mirror33/reachability",
        method="POST",
    )
    assert response.status == 200
    payload = json.loads(response.body)
    assert payload["ok"] is True
    assert payload["reachable"] is True
    assert payload["integrity"] == "ok"
    assert payload["mirrorUrl"] == ORIGIN + "/forkmesh/forkmesh.git"
    other = _get(
        dashboard_app,
        "/api/repo/forkmesh/forkmesh/mirrors/other-node/reachability",
        method="POST",
    )
    assert other.status == 404


def test_api_unknown_repo_and_route_are_not_found(dashboard_app):
    assert _get(dashboard_app, "/api/repo/forkmesh/forkmesh/mirrors").status == 200
    assert _get(dashboard_app, "/api/repo/nobody/nothing/tree").status == 404
    assert _get(dashboard_app, "/api/nonsense").status == 404


# ---------------------------------------------------------------------------
# control-plane routes still work; dashboard is opt-in
# ---------------------------------------------------------------------------


def test_control_routes_are_untouched_by_dashboard(dashboard_app):
    manifest = _get(dashboard_app, "/forkmesh-mirror.json")
    assert manifest.status == 200
    assert manifest.content_type == "application/json; charset=utf-8"
    assert manifest.headers.get("Content-Security-Policy") is None

    health = _get(dashboard_app, "/health")
    assert health.status == 200

    # The public Smart HTTP clone route is unaffected.
    info_refs = _get(
        dashboard_app,
        "/forkmesh/forkmesh.git/info/refs?service=git-upload-pack",
    )
    assert info_refs.status == 200
    assert info_refs.content_type == "application/x-git-upload-pack-advertisement"


def test_dashboard_disabled_by_default(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config = gateway.load_config(
        _write_config(tmp_path, bare, dashboard=False)
    )
    app = gateway.GatewayApplication(
        config,
        verifier=_Verifier(),
        health_signer=_Signer(),
        clock_ms=lambda: NOW,
    )
    try:
        assert app.dashboard is None
        # Without the dashboard, the website paths fall through to the normal
        # 404/route-not-found control-plane behavior.
        assert _get(app, "/dashboard.js").status == 404
        assert _get(app, "/api/repositories").status == 404
        # But the Smart HTTP clone route still works.
        info_refs = _get(
            app,
            "/forkmesh/forkmesh.git/info/refs?service=git-upload-pack",
        )
        assert info_refs.status == 200
    finally:
        app.close()


def test_dashboard_config_requires_a_known_public_assets_repository(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config = json.loads(
        _write_config(tmp_path, bare).read_text(encoding="utf-8")
    )
    config["dashboard"]["assetsRepository"] = "forkmesh/absent"
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="assetsRepository"):
        gateway.load_config(config_path)


def test_dashboard_ssh_origin_must_be_ssh_git_url(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config = json.loads(
        _write_config(tmp_path, bare).read_text(encoding="utf-8")
    )
    config["dashboard"]["sshOrigin"] = "https://ssh.forkmesh.example"
    config_path = tmp_path / "gateway.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="sshOrigin"):
        gateway.load_config(config_path)


def test_dashboard_replica_requires_canonical_origin_and_safe_path(tmp_path):
    bare = _make_assets_repository(tmp_path)
    config_path = _write_config(tmp_path, bare)
    config = json.loads(config_path.read_text(encoding="utf-8"))
    config["dashboard"]["replicaPath"] = str(tmp_path / "replica.json")
    config_path.write_text(json.dumps(config), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="canonicalOrigin"):
        gateway.load_config(config_path)

    config["dashboard"]["canonicalOrigin"] = "https://app.forkmesh.example"
    config["dashboard"]["replicaPath"] = "relative/replica.json"
    config_path.write_text(json.dumps(config), encoding="utf-8")
    with pytest.raises(gateway.GatewayError, match="replicaPath"):
        gateway.load_config(config_path)
