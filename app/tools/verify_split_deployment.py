#!/usr/bin/env python3

import json
import os
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


APP = os.environ.get("DEPLOY_VERIFY_URL", "https://app.forkmesh.com").rstrip("/")
WWW = os.environ.get("DEPLOY_VERIFY_WWW_URL", "https://forkmesh.com").rstrip("/")
WORLD = os.environ.get("DEPLOY_VERIFY_WORLD_URL", "https://world.forkmesh.com").rstrip("/")
PUBLIC_SNAPSHOT_PATHS = (
    "/api/repositories",
    "/api/network/overview",
    "/api/repo/forkmesh/forkmesh/mirrors",
    "/api/status",
    "/api/status/world",
)


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, request, file_pointer, code, message, headers, url):
        return None


OPENER = build_opener(NoRedirect)


def request(url, *, method="GET", headers=None, body=None):
    request_headers = {
        "User-Agent": "forkmesh-deploy-verify/1.0",
        "Accept": "*/*",
    }
    request_headers.update(headers or {})
    item = Request(url, data=body, method=method, headers=request_headers)
    try:
        with OPENER.open(item, timeout=25) as response:
            return response.status, dict(response.headers.items()), response.read()
    except HTTPError as error:
        return error.code, dict(error.headers.items()), error.read()
    except URLError as error:
        raise RuntimeError(f"{url}: {error.reason}") from error


def header(headers, name):
    wanted = name.lower()
    return next((value for key, value in headers.items() if key.lower() == wanted), "")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def www_origins():
    alias = os.environ.get("DEPLOY_VERIFY_WWW_ALIAS_URL")
    if alias is None and WWW == "https://forkmesh.com":
        alias = "https://forkmesh.com"
    origins = []
    for candidate in (WWW, (alias or "").rstrip("/")):
        if candidate and candidate not in origins:
            origins.append(candidate)
    return origins


def origin_key(value):
    parsed = urlsplit(value)
    port = parsed.port
    if port is None:
        port = 443 if parsed.scheme.lower() == "https" else 80
    return parsed.scheme.lower(), (parsed.hostname or "").lower(), port


def verify_world_handoff(origin):
    for path in ("/world", "/world/"):
        status, headers, _ = request(origin + path, method="HEAD")
        require(
            status in (301, 302, 307, 308),
            f"{origin}{path} returned {status}; expected a redirect to World",
        )
        location = header(headers, "location")
        require(location, f"{origin}{path} redirect omitted Location")
        destination = urljoin(origin + "/", location)
        require(
            origin_key(destination) == origin_key(WORLD),
            f"{origin}{path} redirected outside the configured World origin",
        )
        require(
            urlsplit(destination).path in ("/", "/world", "/world/"),
            f"{origin}{path} redirected to a noncanonical World path",
        )


def verify_blog(origin):
    status, _, body = request(origin + "/blog")
    require(status == 200, f"{origin}/blog returned {status}")
    text = body.decode("utf-8", "replace")
    require(
        "<title>Blog · ForkMesh</title>" in text
        and 'href="/blog/rss.xml"' in text
        and 'href="/blog/one-app-one-mesh/"' in text,
        f"{origin}/blog omitted its title, RSS link, or release article",
    )

    status, headers, body = request(origin + "/blog/rss.xml")
    require(status == 200, f"{origin}/blog/rss.xml returned {status}")
    require(
        "xml" in header(headers, "content-type").lower()
        and b"<rss" in body[:512],
        f"{origin}/blog/rss.xml was not an RSS document",
    )

    status, _, body = request(origin + "/blog/one-app-one-mesh/")
    article = body.decode("utf-8", "replace")
    require(status == 200, f"{origin} release article returned {status}")
    require(
        "One app, one mesh" in article and "<h1" in article,
        f"{origin} release article body was incomplete",
    )


def verify_login():
    suffix = "/login?next=%2Fdashboard%3Ftab%3Drepos"
    status, _, body = request(APP + suffix)
    require(status == 200, f"App login page returned {status}")
    text = body.decode("utf-8", "replace")
    require(
        "<title>Log in · ForkMesh</title>" in text
        and 'id="login-title">Welcome back</h1>' in text,
        "App login page omitted its title or form heading",
    )
    script_match = re.search(r'<script src="(/login\.js\?v=[0-9a-f]+)"', text)
    require(script_match is not None, "App login page omitted its versioned script")
    status, headers, _ = request(APP + script_match.group(1))
    require(status == 200, f"App login script returned {status}")
    require(
        "javascript" in header(headers, "content-type").lower(),
        "App login script had the wrong content type",
    )

    slash_suffix = "/login/?next=%2Fdashboard%3Ftab%3Drepos"
    status, headers, _ = request(APP + slash_suffix)
    require(status in (301, 302, 307, 308), "App login slash was not canonicalized")
    destination = urlsplit(urljoin(APP + "/", header(headers, "location")))
    require(
        destination.path == "/login"
        and destination.query == "next=%2Fdashboard%3Ftab%3Drepos",
        "App login slash redirect did not preserve the next query",
    )

    status, headers, _ = request(APP + "/login.html")
    require(status == 404, "App login.html did not stay unavailable")
    require(not header(headers, "location"), "App login.html unexpectedly redirected")

    status, headers, _ = request(
        APP + "/api/accounts/login",
        method="POST",
        headers={"content-type": "application/json"},
        body=b"{",
    )
    require(status == 400, f"App malformed login request returned {status}")
    require(not header(headers, "location"), "App login API unexpectedly redirected")
    require(not header(headers, "set-cookie"), "Malformed login set a session cookie")


def verify_app_control_takeover():
    probes = (
        ("/", 308),
        ("/health", 200),
        ("/api/mirrors/https", 200),
    )
    for path, expected_status in probes:
        status, headers, body = request(APP + path)
        require(
            status == expected_status,
            f"App control takeover probe {path} returned {status}",
        )
        require(
            header(headers, "x-forkmesh-worker") == "app"
            and not header(headers, "x-forkmesh-edge-control"),
            f"App control takeover probe {path} still traversed legacy edge control",
        )
        if path == "/":
            require(
                urlsplit(urljoin(APP + "/", header(headers, "location"))).path
                == "/dashboard",
                "App root takeover probe did not preserve the dashboard redirect",
            )
        elif path == "/api/mirrors/https":
            identity = json.loads(body.decode("utf-8"))
            require(
                identity.get("ok") is True
                and re.fullmatch(
                    r"[A-Za-z0-9_-]{43}",
                    str(identity.get("routerPublicKey") or ""),
                ) is not None,
                "App mirror identity takeover probe was invalid",
            )


def main():
    status, version_headers, body = request(APP + "/api/version")
    version = json.loads(body.decode("utf-8"))
    require(status == 200 and version.get("worker") == "app", "App version probe failed")
    require(
        header(version_headers, "x-forkmesh-worker") == "app"
        and not header(version_headers, "x-forkmesh-edge-control"),
        "App version probe did not prove exclusive consolidated ownership",
    )
    deploy_fingerprint = str(version.get("deployFingerprint") or "")
    require(
        len(deploy_fingerprint) == 64
        and all(character in "0123456789abcdef" for character in deploy_fingerprint),
        "App version probe omitted its deploy fingerprint",
    )
    verify_app_control_takeover()

    for path in PUBLIC_SNAPSHOT_PATHS:
        status, headers, _ = request(APP + path)
        require(status == 200, f"public snapshot {path} returned {status}")
        require(
            header(headers, "x-forkmesh-worker") == "app",
            f"public snapshot {path} omitted the App ownership marker",
        )
        require(
            not header(headers, "x-forkmesh-edge-control"),
            f"public snapshot {path} still traversed legacy edge control",
        )
        require(
            header(headers, "x-forkmesh-public-snapshot") == "fresh",
            f"public snapshot {path} was not fresh",
        )
        require(
            header(headers, "x-forkmesh-snapshot-fingerprint")
            == deploy_fingerprint,
            f"public snapshot {path} belongs to another release",
        )
    for url, extra_headers in (
        (APP + "/api/status?fresh=post-deploy-verification", {}),
        (APP + "/api/status", {
            "authorization": "Bearer post-deploy-verification",
        }),
    ):
        status, headers, _ = request(url, headers=extra_headers)
        require(status == 200, f"snapshot bypass probe returned {status}")
        require(
            not header(headers, "x-forkmesh-public-snapshot"),
            "snapshot bypass probe did not reach the Python App",
        )

    for origin, role in ((WWW, "www"), (WORLD, "world")):
        status, headers, _ = request(origin + "/", method="HEAD")
        require(status == 200, f"{role} root returned {status}")
        require(header(headers, "x-forkmesh-worker") == role, f"{role} ownership marker missing")

    for origin in www_origins():
        verify_world_handoff(origin)
        verify_blog(origin)
    verify_login()

    status, headers, body = request(
        WWW + "/api/version",
        method="POST",
        headers={"content-type": "application/json"},
        body=b"{}",
    )
    require(status in (200, 400, 405), "www POST did not reach the App service binding")
    if status == 200:
        marker = json.loads(body.decode("utf-8"))
        require(marker.get("worker") == "app", "www POST reached the wrong Worker")
    require(not header(headers, "location"), "www POST was redirected instead of proxied")

    status, headers, _ = request(
        APP + "/api/version",
        method="OPTIONS",
        headers={
            "origin": WORLD,
            "access-control-request-method": "GET",
            "access-control-request-headers": "authorization,content-type",
        },
    )
    require(status == 204, "allowed CORS preflight failed")
    require(header(headers, "access-control-allow-origin") == WORLD, "allowed CORS origin was not reflected")

    _, headers, _ = request(
        APP + "/api/version",
        method="OPTIONS",
        headers={
            "origin": "https://untrusted.invalid",
            "access-control-request-method": "GET",
        },
    )
    require(not header(headers, "access-control-allow-origin"), "untrusted CORS origin was allowed")

    for origin in (APP, WWW):
        status, headers, _ = request(
            origin + "/api/world/ws",
            headers={"connection": "Upgrade", "upgrade": "websocket"},
        )
        require(status in (400, 401, 403, 426), f"WebSocket boundary returned unexpected {status} via {origin}")
        require(not header(headers, "location"), f"WebSocket boundary redirected via {origin}")

    print("Split deployment verification passed: App API/login, www blog/RSS/service binding and World handoff, World, CORS, and WebSocket boundaries.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
