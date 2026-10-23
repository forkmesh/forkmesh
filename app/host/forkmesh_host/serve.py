"""``python -m forkmesh_host serve``: run the Worker app under uvicorn.

    python -m forkmesh_host serve --port 8080 [--no-build] [--no-cron]
        [--env-file PATH] [--db sqlite:PATH|rqlite:URL|module:callable]
        [--data-dir DIR] [--migrate] [--no-assets]
    python -m forkmesh_host build          # stage app/src_build and app/dist
    python -m forkmesh_host migrate        # apply app/migrations (sqlite DB)
    python -m forkmesh_host rss            # import everything, print VmRSS

One process, one event loop (uvloop + httptools when installed).
"""

from __future__ import annotations

import argparse
import asyncio
import gc
import os
import subprocess
import sys
from pathlib import Path

from . import paths

# The staged modules, the dashboard bundle and the site pages. The dashboard
# and site builds are what fill app/dist (ASSETS); the Python staging alone is
# enough to answer API routes. The last step writes the brotli and gzip
# copies StaticAssets answers with (tools/precompress_assets.py).
BUILD_STEPS = (
    ("tools/build_dashboard_assets.py",),
    ("tools/build_worker_python.py",),
    ("tools/build_site_assets.py", "app"),
    ("tools/precompress_assets.py", "dist"),
)


def build(python_only=False):
    steps = BUILD_STEPS[1:2] if python_only else BUILD_STEPS
    for step in steps:
        print("build: %s" % " ".join(step), flush=True)
        subprocess.run([sys.executable, *step], cwd=paths.APP_DIR, check=True)


def needs_build():
    entry = paths.SRC_BUILD_DIR / "entry.py"
    if not entry.is_file():
        return True
    newest = max(
        path.stat().st_mtime
        for root in (paths.APP_DIR / "src", paths.REPO_DIR / "world" / "src")
        for path in root.glob("*.py"))
    return newest > entry.stat().st_mtime


def _config(args):
    from .env import HostConfig
    config = HostConfig()
    if args.env_file:
        config.env_file = args.env_file
    if args.data_dir:
        config.data_dir = Path(args.data_dir)
    if args.db:
        config.environ["FORKMESH_HOST_DB"] = args.db
    return config


def _migrate(runtime):
    from .d1_sqlite import D1Sqlite, apply_app_schema, apply_migrations
    if isinstance(runtime.env.DB, D1Sqlite):
        applied = apply_migrations(runtime.env.DB,
                                   paths.APP_DIR / "migrations")
        apply_app_schema(runtime.env.DB, runtime.entry)
        print("migrations: %d applied (+ app schema)" % len(applied),
              flush=True)
    else:
        print("migrations: DB is not the sqlite fallback; use "
              "tools/rqlite_migrate.py", flush=True)


def make_app(args):
    from .asgi import HostASGI
    from .runtime import HostRuntime
    runtime = HostRuntime(_config(args))
    if getattr(args, "migrate", False):
        _migrate(runtime)
    if getattr(args, "warm", False):
        runtime.import_lazy_modules()
    gc.collect()
    gc.freeze()
    return HostASGI(runtime, serve_assets=not args.no_assets,
                    cron=not args.no_cron)


def cmd_serve(args):
    if not args.no_build and needs_build():
        build(python_only=(paths.APP_DIR / "dist").is_dir())
    import uvicorn
    from . import vault
    from .ws_protocol import DrainingWebSocketProtocol, stopping_protocol
    if vault.enabled(os.environ):
        vault.harden_process()
    app = make_app(args)
    loop = "auto"
    try:
        import uvloop  # noqa: F401
        loop = "uvloop"
    except ImportError:
        pass
    try:
        from uvicorn.protocols.http.httptools_impl import (
            HttpToolsProtocol as http)
    except ImportError:
        from uvicorn.protocols.http.h11_impl import H11Protocol as http
    stopping = getattr(getattr(getattr(app, "runtime", None), "env", None),
                       "HOST_STOPPING", None)
    if stopping is not None:
        http = stopping_protocol(http, stopping)
    uvicorn.run(app, host=args.host, port=args.port, loop=loop, http=http,
                ws=DrainingWebSocketProtocol, lifespan="on", workers=1,
                log_level=args.log_level, access_log=args.access_log,
                proxy_headers=False, server_header=False,
                **server_limits(os.environ))
    return 0


def _positive(environ, name, default):
    try:
        value = int(environ.get(name, "") or default)
    except ValueError:
        return default
    return value if value > 0 else default


def server_limits(environ):
    """uvicorn's resource limits (audit 2026-09-24, H6). Caddy in front
    enforces header size and client read timeouts; these bound what reaches
    this one process: concurrent connections+tasks (WebSockets included;
    uvicorn answers 503 past it), one incomplete HTTP event (h11), idle
    keep-alive, and a WebSocket message. Request bodies are capped in the
    ASGI layer (request_guard.py)."""
    return {
        "limit_concurrency": _positive(
            environ, "FORKMESH_HOST_LIMIT_CONCURRENCY", 512),
        "h11_max_incomplete_event_size": 64 * 1024,
        "timeout_keep_alive": _positive(
            environ, "FORKMESH_HOST_KEEPALIVE_S", 15),
        "ws_max_size": 4 * 1024 * 1024,
    }


def cmd_migrate(args):
    from .runtime import HostRuntime
    _migrate(HostRuntime(_config(args)))
    return 0


def cmd_rss(args):
    from .runtime import HostRuntime, vm_rss_kib
    before = vm_rss_kib()
    runtime = HostRuntime(_config(args))
    after_import = vm_rss_kib()
    loaded = runtime.import_lazy_modules()
    import gc as _gc
    _gc.collect()
    after_lazy = vm_rss_kib()
    print("VmRSS interpreter=%d KiB entry+env=%d KiB "
          "all-lazy(%d modules)=%d KiB" % (before, after_import, loaded,
                                           after_lazy))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(prog="python -m forkmesh_host")
    sub = parser.add_subparsers(dest="command", required=True)

    def common(p):
        p.add_argument("--env-file")
        p.add_argument("--data-dir")
        p.add_argument("--db", help="sqlite:PATH | rqlite:URL | module:fn")

    serve = sub.add_parser("serve")
    common(serve)
    serve.add_argument("--host", default=os.environ.get(
        "FORKMESH_HOST_BIND", "127.0.0.1"))
    serve.add_argument("--port", type=int, default=int(os.environ.get(
        "FORKMESH_HOST_PORT", "8080")))
    serve.add_argument("--no-build", action="store_true")
    serve.add_argument("--no-cron", action="store_true",
                       default=os.environ.get("FORKMESH_HOST_CRON") == "0")
    serve.add_argument("--no-assets", action="store_true",
                       default=os.environ.get(
                           "FORKMESH_HOST_SERVE_ASSETS") == "0")
    serve.add_argument("--migrate", action="store_true",
                       help="apply app/migrations to the sqlite DB first")
    serve.add_argument("--warm", action="store_true",
                       help="import every lazy route module before serving")
    serve.add_argument("--log-level", default="info")
    serve.add_argument("--access-log", action="store_true")
    serve.set_defaults(func=cmd_serve)

    sub.add_parser("build").set_defaults(
        func=lambda args: build() or 0)
    migrate = sub.add_parser("migrate")
    common(migrate)
    migrate.set_defaults(func=cmd_migrate)
    rss = sub.add_parser("rss")
    common(rss)
    rss.set_defaults(func=cmd_rss)
    from . import ops
    ops.add_parser(sub, common)
    from . import instance_bootstrap
    instance_bootstrap.add_parser(sub, common)
    from . import instance_repo
    instance_repo.add_parser(sub, common)
    from . import pull_resync
    pull_resync.add_parser(sub, common)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
