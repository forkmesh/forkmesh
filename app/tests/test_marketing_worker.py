"""Marketing Worker ownership and compatibility contracts."""

import tomllib
from pathlib import Path


PROJECT = Path(__file__).resolve().parents[2]
WWW = PROJECT / "www"
APP = PROJECT / "app"
WRANGLER = tomllib.loads((WWW / "wrangler.toml").read_text(encoding="utf-8"))
WORKER = (WWW / "worker.js").read_text(encoding="utf-8")
DEPLOY = (APP / "deploy.sh").read_text(encoding="utf-8")
REDIRECTS = (WWW / "public" / "_redirects").read_text(encoding="utf-8")


def test_www_worker_owns_only_official_marketing_domains():
    assert "name" not in WRANGLER
    assert "routes" not in WRANGLER
    assert WRANGLER["assets"]["directory"] == "./dist"
    # Landing pages pass through the Worker only so acquisition attribution
    # can observe them server-side (docs/attribution.md); the Worker still
    # answers from the static asset. Blog and docs remain static assets.
    assert "/" in WRANGLER["assets"]["run_worker_first"]
    assert "/blog" not in WRANGLER["assets"]["run_worker_first"]
    assert "/blog/*" not in WRANGLER["assets"]["run_worker_first"]
    assert "/docs/*" not in WRANGLER["assets"]["run_worker_first"]
    assert WRANGLER["vars"]["APP_ORIGIN"] == "https://app.forkmesh.com"
    assert WRANGLER["vars"]["WORLD_ORIGIN"] == "https://world.forkmesh.com"
    production = tomllib.loads((WWW / "wrangler.production.toml").read_text(encoding="utf-8"))
    assert production["name"] == "forkmesh-www"
    assert {route["pattern"] for route in production["routes"]} == {
        "forkmesh.com",
        "www.forkmesh.com",
    }
    # Next has no App Worker: worker.js fetches APP_ORIGIN over the internet
    # (deploy/core/edge_client.md), so there is no service binding.
    assert "services" not in WRANGLER


def test_clean_marketing_urls_are_backed_by_www_assets():
    for asset in (
        "index.html",
        "blog.html",
        "docs/index.html",
    ):
        assert (WWW / "public" / asset).is_file()
    assert not (WWW / "public" / "world").exists()
    assert not (WWW / "public" / "dashboard").exists()
    assert sorted((WWW / "public" / "blog").glob("*/index.html"))
    # /pricing is retired: the asset router sends it to the home page's
    # pricing section rather than to a page of its own.
    assert not (WWW / "public" / "pricing.html").exists()
    assert "/pricing /#pricing 308" in REDIRECTS
    assert "/pricing.html /#pricing 308" in REDIRECTS
    assert "/blog /blog.html 200" not in REDIRECTS


def test_api_and_protocol_requests_are_proxied_to_app():
    assert 'if (path === "/api")' in WORKER
    assert 'if (path.startsWith("/api/"))' in WORKER
    assert "return proxyToApp(request, env);" in WORKER
    assert "env.APP.fetch" not in WORKER
    assert "return appFetch(new Request(forwarded, { headers, redirect: \"manual\" }), env);" \
        in WORKER
    assert 'headers.set("x-forkmesh-public-profile-proxy", "forkmesh.com")' \
        in WORKER
    assert "isGitProtocol(path) || isBackendProtocol(path)" in WORKER


def test_product_and_world_routes_leave_the_marketing_worker():
    assert 'path === "/world" || path.startsWith("/world/")' in WORKER
    assert "redirect(env.WORLD_ORIGIN" in WORKER
    assert 'path === "/dashboard" || path.startsWith("/dashboard/")' in WORKER
    assert "redirect(env.APP_ORIGIN" in WORKER
    assert "/world https://world.forkmesh.com/ 308" in REDIRECTS
    assert "/dashboard https://app.forkmesh.com/dashboard 308" in REDIRECTS


def test_www_static_directories_are_reserved_before_repo_shortcuts():
    assert 'const WWW_PREFIXES = ["/assets/", "/favicon/", "/blog/", "/docs/"]' in WORKER
    assert "!WWW_PREFIXES.some((prefix) => path.startsWith(prefix))" in WORKER


def test_deploy_coordinator_checks_www_as_an_independent_target():
    assert 'world|www)' in DEPLOY
    assert '"$DEPLOY_SELF" app' in DEPLOY
    assert '"$DEPLOY_SELF" world' in DEPLOY
    assert '"$DEPLOY_SELF" www' in DEPLOY
    assert 'DEPLOY_VERIFY_WWW_URL:-https://forkmesh.com' in DEPLOY
    assert '_status_check "${base%/}/pricing" 308' in DEPLOY
    usage = next(line for line in DEPLOY.splitlines() if "Usage: $0 [" in line)
    assert "|world|www|" in usage
