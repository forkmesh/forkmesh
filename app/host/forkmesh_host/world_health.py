"""``GET /health/mobile`` on world.next: world/worker.js mobileWorldHealth().

On Cloudflare the World Worker answered this from its static-asset binding.
On Next, Caddy serves world.next from the release's ``world/dist`` and routes
only ``/health/mobile`` to the app (deploy/core/caddy_routes.py); the host
answers it here, before the Worker entry, when the request's Host is the
World origin's host. It checks the same asset contracts against the files in
``world/dist`` -- the exact files Caddy serves -- and answers with the same
JSON and headers, which the app's ``world_mobile`` status probe
(app/src/entry.py ``_mobile_world_status_probe``) requires:
``x-forkmesh-worker: world`` and ``x-forkmesh-deploy-fingerprint``.

``FORKMESH_HOST_WORLD_DIST`` overrides the directory (default: the release's
``world/dist`` next to ``app/``).
"""

from __future__ import annotations

import json
import mimetypes
import os
import urllib.parse
from pathlib import Path

from . import paths

HEALTH_PATH = "/health/mobile"

# world/worker.js MOBILE_WORLD_RUNTIME_ASSETS:
# (path, contentType substring, minimumBytes, maximumBytes).
# /world/world.js is a small entry shell in a staged build (the authored
# runtime is published as world-runtime.js and imported by the shell), so its
# bounds are the shell's, and RUNTIME_DEPENDENCIES below makes the same item
# also require the runtime it loads. The probe in entry.py expects exactly
# these five paths, so the runtime is folded into the world.js result.
MOBILE_WORLD_RUNTIME_ASSETS = (
    ("/world/index.html", "text/html", 3_000, 64 * 1024),
    ("/world/world.js", "javascript", 1024, 64 * 1024),
    ("/world/world-scene.js", "javascript", 1024, 4 * 1024 * 1024),
    ("/world/vendor/three-0.184.0/three.module.min.js", "javascript",
     256 * 1024, 512 * 1024),
    ("/world/vendor/three-0.184.0/three.core.min.js", "javascript",
     256 * 1024, 512 * 1024),
)

RUNTIME_DEPENDENCIES = {
    "/world/world.js": ("/world/world-runtime.js", "javascript",
                        512 * 1024, 2 * 1024 * 1024),
}

# What Caddy's file_server (and Workers Static Assets) send for these types.
_TYPES = {".html": "text/html; charset=utf-8",
          ".js": "text/javascript; charset=utf-8",
          ".mjs": "text/javascript; charset=utf-8"}


def world_dist():
    configured = os.environ.get("FORKMESH_HOST_WORLD_DIST")
    if configured:
        return Path(configured)
    return paths.REPO_DIR / "world" / "dist"


def content_type(path: Path) -> str:
    known = _TYPES.get(path.suffix.lower())
    if known:
        return known
    guessed, _ = mimetypes.guess_type(path.name)
    return guessed or "application/octet-stream"


def check_asset(root: Path, contract):
    """One checkMobileWorldAsset() result, from the file on disk."""
    path, kind, minimum, maximum = contract
    try:
        file = (root / path.lstrip("/")).resolve()
        if root.resolve() not in file.parents or not file.is_file():
            return {"path": path, "ok": False, "status": 404,
                    "reason": "%s returned HTTP 404" % path}
        if kind not in content_type(file).lower():
            return {"path": path, "ok": False, "status": 200,
                    "reason": "%s returned an invalid content type" % path}
        size = file.stat().st_size
        if size > 0 and (size < minimum or size > maximum):
            return {"path": path, "ok": False, "status": 200, "bytes": size,
                    "reason": "%s returned an unexpected byte length" % path}
        return {"path": path, "ok": True, "status": 200,
                "bytes": size or None}
    except OSError as error:
        return {"path": path, "ok": False, "status": 0,
                "reason": "%s could not be loaded: %s"
                % (path, str(error)[:120])}


def check_with_dependency(root: Path, contract):
    item = check_asset(root, contract)
    dependency = RUNTIME_DEPENDENCIES.get(contract[0])
    if item["ok"] and dependency is not None:
        runtime = check_asset(root, dependency)
        if not runtime["ok"]:
            item = dict(item, ok=False, reason=runtime["reason"])
    return item


def health_payload(root=None):
    root = Path(root) if root is not None else world_dist()
    assets = [check_with_dependency(root, contract)
              for contract in MOBILE_WORLD_RUNTIME_ASSETS]
    failure = next((item for item in assets if not item["ok"]), None)
    return {"ok": failure is None, "check": "mobile-world-startup",
            "profile": "mobile", "sameOriginRuntime": True,
            "threeRevision": "184", "assets": assets,
            "reason": failure["reason"] if failure else None}


def world_hostname(env):
    origin = str(getattr(env, "WORLD_ORIGIN", "") or "")
    return (urllib.parse.urlsplit(origin).hostname or "").lower()


def matches(path, host_header, env):
    if path != HEALTH_PATH:
        return False
    world = world_hostname(env)
    host = str(host_header or "").split(":")[0].strip().lower()
    return bool(world) and host == world


def response(method, env, root=None):
    """(status, headers, body) for the ASGI front end."""
    fingerprint = str(getattr(env, "DEPLOY_FINGERPRINT", "") or "")
    if method not in ("GET", "HEAD"):
        return 405, [("allow", "GET, HEAD"),
                     ("content-type", "text/plain; charset=utf-8")], \
            b"Method not allowed"
    payload = health_payload(root)
    headers = [("cache-control", "no-store"),
               ("content-type", "application/json; charset=utf-8"),
               ("x-forkmesh-worker", "world"),
               ("x-forkmesh-deploy-fingerprint", fingerprint)]
    body = b"" if method == "HEAD" else json.dumps(
        payload, separators=(",", ":")).encode()
    return (200 if payload["ok"] else 503), headers, body
