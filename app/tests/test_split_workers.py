import hashlib
import json
import posixpath
import re
import shutil
import subprocess
import sys
import tomllib
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
APP_ROOT = REPOSITORY / "app"
APP_RUNTIME = APP_ROOT / "src" / "app_runtime.py"
if str(APP_ROOT / "src") not in sys.path:
    sys.path.insert(0, str(APP_ROOT / "src"))

import dashboard_shell  # noqa: E402


STAGED_REFERENCE_RE = re.compile(
    r'(?:src|href)="/([\w.-]+\.(?:js|css))(?:\?v=([^"]+))?"'
)
STAGED_ASSIGNMENT_RE = re.compile(
    r'=\s*"/([\w.-]+\.js)(?:\?v=([^"]+))?"'
)
STAGED_TEMPLATE_ASSIGNMENT_RE = re.compile(
    r'const\s+version\s*=\s*"([^"]+)";\s*'
    r'[\w$.]+\.src\s*=\s*`/([\w.-]+\.js)\?v=\$\{version\}`'
)
MINIFIED_TEMPLATE_ASSIGNMENT_RE = re.compile(
    r'([\w$]+)="([0-9a-f]{12})";[\w$.]+\.src=`/([\w.-]+\.js)\?v=\$\{\1\}`'
)
STAGED_MODULE_URL_RE = re.compile(
    r"""["'`]((?:\.{1,2}/|/)[\w./-]+?\.js)\?v=([0-9a-f]{12})"""
)


def config(unit):
    return tomllib.loads(
        (REPOSITORY / unit / "wrangler.toml").read_text(encoding="utf-8")
    )


def test_public_workers_and_consolidated_app_have_disjoint_runtime_roles():
    app = config("app")
    www = config("www")
    world = config("world")

    assert app["name"] == "forkmesh-next-app"
    assert app["main"] == "src_build/entry.py"
    assert app["vars"]["WORKER_ROLE"] == "app"
    assert app["vars"]["API_ORIGIN"] == "https://app.forkmesh.com"
    assert app["routes"] == [
        {"pattern": "app.next.forkmesh.com", "custom_domain": True},
    ]
    assert app["assets"]["directory"] == "./dist"
    assert app["triggers"]["crons"] == ["* * * * *"]
    assert "durable_objects" in app and "migrations" in app
    assert [database["binding"] for database in app["d1_databases"]] == ["DB"]
    assert not (APP_ROOT / "edge-control/worker.js").exists()
    assert not (APP_ROOT / "edge-control/wrangler.toml").exists()

    expected = {
        "www": (www, None, []),
        "world": (world, "forkmesh-next-world", ["world.next.forkmesh.com"]),
    }
    for unit, (manifest, name, hosts) in expected.items():
        assert manifest.get("name") == name
        assert manifest["main"] == "worker.js"
        assert manifest["assets"]["directory"] == "./dist"
        assert isinstance(manifest["assets"]["run_worker_first"], list)
        # World owns "/" as an application; www now also runs first on "/"
        # purely so acquisition attribution can observe the landing page
        # server-side (docs/attribution.md) — it still serves the static
        # asset and stays free of every backend binding checked below.
        assert "/" in manifest["assets"]["run_worker_first"]
        assert [route["pattern"] for route in manifest.get("routes", [])] == hosts
        assert all(route["custom_domain"] for route in manifest.get("routes", []))
        for backend_only in (
            "ai",
            "d1_databases",
            "durable_objects",
            "kv_namespaces",
            "migrations",
            "triggers",
        ):
            assert backend_only not in manifest, (unit, backend_only)

    www_worker_routes = www["assets"]["run_worker_first"]
    for route in ("/api", "/api/*"):
        assert route in www_worker_routes
    for route in ("/world", "/world/*", "/dashboard"):
        assert route not in www_worker_routes
    assert "/styles.css" not in www_worker_routes
    world_worker_routes = world["assets"]["run_worker_first"]
    for route in (
        "/", "/health/mobile", "/world", "/world/", "/api", "/api/*"
    ):
        assert route in world_worker_routes
    for route in (
        "/api-client.js",
        "/dashboard-chat.js",
        "/assets/*",
        "/favicon/*",
    ):
        assert route in world_worker_routes
    for route in ("!/assets/bot-avatars/*", "!/assets/world/*"):
        assert route in world_worker_routes
    for asset in (
        "/world/world.js",
        "/world/world.css",
        "/world/world-scene.js",
        "/assets/logo.png",
    ):
        assert asset not in world_worker_routes

    # WWW reaches the App over APP_ORIGIN (no App Worker exists on Next).
    assert "services" not in www
    assert world["services"] == [
        {"binding": "APP", "service": "forkmesh-next-app"}
    ]


def test_obsolete_api_unit_and_duplicate_manifests_are_gone():
    assert not (REPOSITORY / "api").exists()
    assert not (APP_ROOT / "wrangler.api.toml").exists()
    assert not (APP_ROOT / "wrangler.www.toml").exists()
    assert not (APP_ROOT / "wrangler.world.toml").exists()


def test_source_assets_are_physically_owned_by_their_worker():
    assert (REPOSITORY / "www/public/index.html").is_file()
    assert (REPOSITORY / "www/public/blog/one-app-one-mesh/index.html").is_file()
    assert (REPOSITORY / "www/public/docs/index.html").is_file()
    assert (REPOSITORY / "www/docs/operations/worker-split.md").is_file()
    assert (REPOSITORY / "app/public/dashboard/repo.html").is_file()
    assert (REPOSITORY / "app/public/login.html").is_file()
    assert (REPOSITORY / "app/public/assets/file-icons/file_type_git.svg").is_file()
    assert (REPOSITORY / "world/public/world/world.js").is_file()
    assert (REPOSITORY / "world/public/qr.js").is_file()
    assert (REPOSITORY / "world/public/security/latest.json").is_file()
    assert not (APP_ROOT / "public/qr.js").exists()
    assert not (APP_ROOT / "public/security/latest.json").exists()
    assert (REPOSITORY / "world/src/world.py").is_file()
    assert (REPOSITORY / "world/src/world_office_tasks.py").is_file()
    assert (REPOSITORY / "world/src/fediverse_mentions_api.py").is_file()
    assert not (APP_ROOT / "src/fediverse_mentions_api.py").exists()
    assert not list((APP_ROOT / "src").glob("world*.py"))
    assert (REPOSITORY / "world/tests/test_world_backend.py").is_file()
    assert (REPOSITORY / "world/tests/test_fediverse_mentions.py").is_file()
    assert not (APP_ROOT / "tests/test_fediverse_mentions.py").exists()
    assert not list((APP_ROOT / "tests").glob("test_world_*.py"))
    assert not (REPOSITORY / "world/public/assets/songs").exists()
    assert not (APP_ROOT / "docs").exists()


def test_staging_is_self_host_complete_without_shipping_official_blog():
    world_source = (REPOSITORY / "world/public/world/world.js").read_text(
        encoding="utf-8"
    )
    world_shell_source = (
        REPOSITORY / "world/public/world/index.html"
    ).read_text(encoding="utf-8")
    assert 'const version = "__FORKMESH_ASSET_VERSION__";' in world_source
    assert 'script.src = `/dashboard-chat.js?v=${version}`;' in world_source
    assert '/api-client.js?v=${version}' in world_shell_source
    subprocess.run(
        [sys.executable, str(APP_ROOT / "tools/build_site_assets.py")],
        cwd=REPOSITORY,
        check=True,
        capture_output=True,
    )
    try:
        source = (REPOSITORY / "www/public/api-client.js").read_bytes()
        for unit in ("app", "www"):
            assert (REPOSITORY / unit / "dist/api-client.js").read_bytes() == source
            assert (REPOSITORY / unit / "dist/auth-page-theme.css").is_file()
            assert (REPOSITORY / unit / "dist/mesh-page-theme.css").is_file()

        app_files = {
            path.relative_to(APP_ROOT / "dist").as_posix()
            for path in (APP_ROOT / "dist").rglob("*")
            if path.is_file()
        }
        assert {
            "api-client.js",
            "blog.html",
            "blog-social.css",
            "blog-social.js",
            "dashboard/index.html",
            "dashboard/repo.html",
            "feature-post.css",
            "index.html",
            "install.sh",
            "uninstall.sh",
            "login.html",
            "signup.html",
            "status.html",
        } <= app_files
        assert not any(path.startswith("blog/") for path in app_files)
        assert not any(path.startswith("assets/blog/") for path in app_files)
        assert "docs/index.html" not in app_files
        assert (REPOSITORY / "www/dist/membership-prompt.js").read_bytes() == (
            APP_ROOT / "public/membership-prompt.js"
        ).read_bytes()

        world_dist = REPOSITORY / "world/dist"
        proxied_app_assets = (
            "account-events.js",
            "api-client.js",
            "chat-apps.js",
            "chat-attachments.js",
            "chat-crypto.js",
            "chat-issue-filing.js",
            "chat-mlkem.js",
            "chat-moderation.js",
            "chat-retention.js",
            "chat-room-transport.js",
            "chat-seal.js",
            "dashboard-chat.js",
            "member-welcome.js",
            "analytics.js",
            "analytics-sdk.js",
        )
        for relative in proxied_app_assets:
            assert not (world_dist / relative).exists()
            assert (APP_ROOT / "dist" / relative).is_file()
        assert not (world_dist / "favicon").exists()
        assert (APP_ROOT / "dist/favicon/favicon.ico").is_file()
        assert (APP_ROOT / "dist/assets/logo.png").is_file()
        assert (APP_ROOT / "dist/assets/fonts/Lato-Regular.woff2").is_file()
        assert (world_dist / "assets/bot-avatars/opus-face.webp").is_file()
        assert (
            world_dist / "assets/world/marketing-table-cube-inlay.webp"
        ).is_file()

        world_files = {
            path.relative_to(world_dist).as_posix()
            for path in world_dist.rglob("*")
            if path.is_file()
        }
        world_source_files = {
            path.relative_to(REPOSITORY / "world/public").as_posix()
            for path in (REPOSITORY / "world/public").rglob("*")
            if path.is_file()
        }
        world_package_files = {"marketplace/manifest.schema.json"}
        world_package_ids = (
            "x-banner-board",
            "reddit-banner-board",
            "forkmesh-sky-office",
        )
        for package_id in world_package_ids:
            source_package = REPOSITORY / "marketplace" / "packages" / package_id
            world_package_files.update(
                "marketplace/packages/{}/{}".format(
                    package_id, path.relative_to(source_package).as_posix()
                )
                for path in source_package.rglob("*")
                # A package's own tests stay out of the public asset upload.
                if path.is_file() and "tests" not in path.relative_to(source_package).parts
            )
        assert (
            REPOSITORY / "marketplace/packages/forkmesh-sky-office/tests"
        ).is_dir()
        assert not (world_dist / "marketplace/packages/forkmesh-sky-office/tests").exists()
        expected_world_files = (
            world_source_files | {"_headers"} | world_package_files
        )
        expected_world_files.remove("world/world-loader.js")
        expected_world_files.add("world/world-runtime.js")
        expected_world_files -= {
            relative
            for relative in expected_world_files
            if relative.startswith("world/scene/")
            and relative not in ("world/scene/quality.js", "world/scene/environment.js")
        }
        expected_world_files |= {
            relative
            for relative in world_files
            if relative.startswith("world/scene/chunks/") and relative.endswith(".js")
        }
        minified_modules = {
            relative
            for relative in expected_world_files
            if relative.endswith(".js")
            and not relative.startswith("world/vendor/")
            and (
                relative.startswith("world/")
                or relative == "qr.js"
                or re.match(r"^marketplace/packages/[^/]+/content/world/", relative)
            )
        }
        expected_world_files |= {relative + ".map" for relative in minified_modules}
        assert world_files == expected_world_files
        world_upload_bytes = sum(
            path.stat().st_size
            for path in world_dist.rglob("*")
            if path.is_file() and path.suffix != ".map"
        )
        source_map_bytes = sum(
            path.stat().st_size for path in world_dist.rglob("*.map")
        )
        assert world_upload_bytes < 5_000_000
        assert source_map_bytes < 7_000_000
        assert (world_dist / "security/latest.json").read_bytes() == (
            REPOSITORY / "world/public/security/latest.json"
        ).read_bytes()
        staged_modules = {
            relative: (world_dist / relative).read_text(encoding="utf-8")
            for relative in minified_modules
        }
        for relative, text in staged_modules.items():
            assert text.rstrip().endswith(
                "//# sourceMappingURL=%s.map" % Path(relative).name
            ), relative
            source_map = json.loads(
                (world_dist / (relative + ".map")).read_text(encoding="utf-8")
            )
            if relative == "world/world-scene.js" or relative.startswith("world/scene/"):
                assert all(
                    source.startswith("world/public/world/")
                    for source in source_map["sources"]
                ), relative
            else:
                assert len(source_map["sources"]) == 1
                assert source_map["sourcesContent"][0]
            for specifier, version in STAGED_MODULE_URL_RE.findall(text):
                if specifier.startswith("/"):
                    target = specifier.lstrip("/")
                else:
                    target = posixpath.normpath(
                        posixpath.join(posixpath.dirname("/" + relative), specifier)
                    ).lstrip("/")
                if target not in staged_modules:
                    continue
                assert version == hashlib.sha256(
                    staged_modules[target].encode("utf-8")
                ).hexdigest()[:12], (relative, specifier)
        assert len((world_dist / "qr.js").read_bytes()) < len(
            (REPOSITORY / "world/public/qr.js").read_bytes()
        )
        for package_id in world_package_ids:
            staged_package = world_dist / "marketplace" / "packages" / package_id
            source_package = REPOSITORY / "marketplace" / "packages" / package_id
            for relative in ("manifest.json", "LICENSE"):
                assert (staged_package / relative).read_bytes() == (
                    source_package / relative
                ).read_bytes()
            assert (staged_package / "content/world/index.js.map").is_file()
        assert (
            world_dist / "marketplace" / "manifest.schema.json"
        ).read_bytes() == (
            REPOSITORY / "marketplace" / "manifest.schema.json"
        ).read_bytes()
        chat_version = hashlib.sha256(
            (APP_ROOT / "public/dashboard-chat.js").read_bytes()
        ).hexdigest()[:12]
        api_version = hashlib.sha256(
            (REPOSITORY / "www/public/api-client.js").read_bytes()
        ).hexdigest()[:12]
        staged_world = (world_dist / "world/world-runtime.js").read_text(
            encoding="utf-8"
        )
        staged_world_loader = (world_dist / "world/world.js").read_text(
            encoding="utf-8"
        )
        staged_world_shell = (world_dist / "world/index.html").read_text(
            encoding="utf-8"
        )
        staged_dashboard = (APP_ROOT / "dist/dashboard/index.html").read_text(
            encoding="utf-8"
        )
        staged_chat = (APP_ROOT / "dist/chat.html").read_text(encoding="utf-8")
        staged_about = (
            REPOSITORY / "www/dist/about.html"
        ).read_text(encoding="utf-8")
        assert [
            (version, name)
            for _, version, name in MINIFIED_TEMPLATE_ASSIGNMENT_RE.findall(staged_world)
        ] == [(chat_version, "dashboard-chat.js")]
        assert re.search(
            r'await import\("\./world-runtime\.js\?v=[0-9a-f]{12}"\)',
            staged_world_loader,
        )
        assert re.search(
            r'src="/world/world\.js\?v=[0-9a-f]{12}"',
            staged_world_shell,
        )
        runtime_import = re.search(
            r'await import\("\./world-runtime\.js\?v=([0-9a-f]{12})"\)',
            staged_world_loader,
        ).group(1)
        assert re.findall(
            r'<link rel="modulepreload" href="/world/world-runtime\.js\?v=([0-9a-f]{12})" />',
            staged_world_shell,
        ) == [runtime_import]
        assert staged_world_shell.index("world-runtime.js?v=") < staged_world_shell.index(
            'src="/world/world.js?v='
        )
        runtime_static_imports = re.findall(
            r"""(?:\bfrom\s*|\bimport\s*)["']\./([A-Za-z0-9_-]+\.js)\?v=([0-9a-f]{12})["']""",
            (world_dist / "world/world-runtime.js").read_text(encoding="utf-8"),
        )
        assert runtime_static_imports
        for name, version in runtime_static_imports:
            assert (
                f'<link rel="modulepreload" href="/world/{name}?v={version}" />'
                in staged_world_shell
            )
        staged_scene = sorted(
            path.relative_to(world_dist / "world/scene").as_posix()
            for path in (world_dist / "world/scene").rglob("*.js")
        )
        assert {"quality.js", "environment.js"} <= set(staged_scene)
        assert all(
            name in ("quality.js", "environment.js") or name.startswith("chunks/")
            for name in staged_scene
        )
        assert len(staged_scene) <= 8
        loader_modules = re.findall(r'"(/world/[^"?]+\.js)\?v=', staged_world_loader)
        assert "/world/world-scene.js" in loader_modules
        assert not [
            href
            for href in loader_modules
            if href.startswith("/world/scene/")
            and not href.startswith("/world/scene/chunks/")
            and href not in ("/world/scene/quality.js", "/world/scene/environment.js")
        ]
        assert len([href for href in loader_modules if href.startswith("/world/scene/")]) <= 8
        assert len(loader_modules) <= 24
        scene_bundle = (world_dist / "world/world-scene.js").read_text(encoding="utf-8")
        for specifier in re.findall(r'from\s*"([^"]+)"', scene_bundle):
            assert specifier.startswith(("/world/", "./scene/chunks/")), specifier
        scene_map = json.loads(
            (world_dist / "world/world-scene.js.map").read_text(encoding="utf-8")
        )
        assert "world/public/world/scene/core.js" in scene_map["sources"]
        assert 512 * 1024 < (world_dist / "world/world-runtime.js").stat().st_size < len(
            world_source.encode("utf-8")
        )
        assert (world_dist / "world/world.js").stat().st_size < 10_000
        assert re.findall(
            r'/api-client\.js\?v=([0-9a-f]{12})', staged_world_shell
        ) == [api_version]
        assert f'/api-client.js?v={api_version}' in staged_dashboard
        assert f'/api-client.js?v={api_version}' in staged_about
        dashboard_version = hashlib.sha256(
            (APP_ROOT / "dist/dashboard.js").read_bytes()
        ).hexdigest()[:12]
        chat_css_version = hashlib.sha256(
            (APP_ROOT / "dist/chat.css").read_bytes()
        ).hexdigest()[:12]
        assert f'/dashboard.js?v={dashboard_version}' in staged_dashboard
        assert f'/chat.css?v={chat_css_version}' in staged_chat

        seen = set()
        for unit in ("app", "www", "world"):
            root = REPOSITORY / unit / "dist"
            versions = dashboard_shell.staged_asset_versions(root)
            for path in sorted(root.rglob("*.html")):
                for name, version in STAGED_REFERENCE_RE.findall(
                    path.read_text(encoding="utf-8")
                ):
                    if name not in versions:
                        continue
                    assert version == versions[name], (unit, path, name)
                    seen.add(name)
            for path in sorted(root.rglob("*.js")):
                if path.name in versions:
                    continue
                source = path.read_text(encoding="utf-8")
                for name, version in STAGED_ASSIGNMENT_RE.findall(source):
                    if name not in versions:
                        continue
                    assert version == versions[name], (unit, path, name)
                    seen.add(name)
                for version, name in STAGED_TEMPLATE_ASSIGNMENT_RE.findall(source):
                    if name not in versions:
                        continue
                    assert version == versions[name], (unit, path, name)
                    seen.add(name)
                for _, version, name in MINIFIED_TEMPLATE_ASSIGNMENT_RE.findall(source):
                    if name not in versions:
                        continue
                    assert version == versions[name], (unit, path, name)
                    seen.add(name)
        assert seen == set(dashboard_shell.CACHE_BUSTED_ASSETS)
    finally:
        for path in (
            REPOSITORY / "app/dist",
            REPOSITORY / "www/dist",
            REPOSITORY / "world/dist",
        ):
            shutil.rmtree(path, ignore_errors=True)


def test_app_root_and_api_landing_support_official_and_self_host_modes():
    runtime = APP_RUNTIME.read_text(encoding="utf-8")
    manifest = config("app")

    assert 'url.path in ("/api", "/api/", "/developers", "/developers/")' in runtime
    assert "_api_metrics.landing_page_response" in runtime
    assert 'getattr(self.env, "SINGLE_WORKER_SITE", "")' in runtime
    assert "return await self._serve_homepage(url)" in runtime
    assert 'url, "dashboard/index.html")' in runtime
    assert manifest["vars"]["SINGLE_WORKER_SITE"] == "false"
    assert "/api" in manifest["assets"]["run_worker_first"]
    assert "/api/*" in manifest["assets"]["run_worker_first"]
    for clean_page in (
        "/login",
        "/ledger",
    ):
        assert clean_page not in manifest["assets"]["run_worker_first"]
        assert (APP_ROOT / "public" / (clean_page[1:] + ".html")).is_file()
    for www_page in ("/status", "/network", "/leaderboards"):
        assert www_page in manifest["assets"]["run_worker_first"]
        assert (APP_ROOT / "public" / (www_page[1:] + ".html")).is_file()
    assert "/signup" in manifest["assets"]["run_worker_first"]
    assert "/signup/" in manifest["assets"]["run_worker_first"]
    assert (APP_ROOT / "public/signup.html").is_file()
    assert manifest["assets"]["html_handling"] == "drop-trailing-slash"


def test_app_redirects_external_content_to_its_owning_worker():
    runtime = APP_RUNTIME.read_text(encoding="utf-8")
    routes = config("app")["assets"]["run_worker_first"]
    static_routes = (APP_ROOT / "src/static_routes.py").read_text(encoding="utf-8")

    assert 'origin_name = (' in runtime
    assert '"WORLD_ORIGIN" if external_owner == "world" else "WWW_ORIGIN"' in runtime
    assert 'return Response("", status=308, headers={"location": destination})' in runtime
    for path in ("/docs", "/docs/*", "/blog", "/blog/*", "/desktop", "/pricing", "/world", "/world/*"):
        assert path in routes
    assert '"docs"' in static_routes
    assert '"blog"' in static_routes
    assert '"world"' in static_routes


def test_world_shell_injects_configured_app_origin_before_api_client():
    worker = (REPOSITORY / "world/worker.js").read_text(encoding="utf-8")
    shell = (REPOSITORY / "world/public/world/index.html").read_text(
        encoding="utf-8"
    )
    client = (REPOSITORY / "www/public/api-client.js").read_text(encoding="utf-8")

    assert 'meta name="forkmesh-api-origin"' in worker
    assert 'meta name="forkmesh-world-deploy"' in worker
    assert "env.APP_ORIGIN" in worker
    assert "env.DEPLOY_FINGERPRINT" in worker
    assert shell.index('src="/api-client.js') < shell.index('src="/world/world.js')
    assert "metadataOrigin()" in client
    assert 'return pathname.startsWith("/api/")' in client
    assert "return env.APP.fetch(request);" in worker


def test_world_delegates_non_world_routes_for_app_owned_routing():
    worker_path = REPOSITORY / "world/worker.js"
    worker = worker_path.read_text(encoding="utf-8")
    runtime = APP_RUNTIME.read_text(encoding="utf-8")

    assert "APP_PATHS" not in worker
    assert "WWW_PATHS" not in worker
    assert "_world_ingress_redirect" in runtime
    assert runtime.index(
        "delegated = self._world_ingress_redirect(request, url)"
    ) < runtime.index("private_access_match = PRIVATE_REPLICA_ACCESS_RE.match")

    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
global.HTMLRewriter = class {
  on() { return this; }
  transform(response) { return response; }
};
const delegated = [];
const assets = [];
const env = {
  APP_ORIGIN: "https://app.forkmesh.com",
  DEPLOY_FINGERPRINT: "abc123",
  APP: {
    async fetch(request) {
      delegated.push(request.url);
      return new Response("app", { status: 308 });
    },
  },
  ASSETS: {
    async fetch(request) {
      assets.push(request.url);
      return new Response("world", { status: 200 });
    },
  },
};
// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// harness routes the global fetch to the stand-in App.
globalThis.fetch = async (input, init) => env.APP.fetch(new Request(input, init));
for (const path of ["/api/world/ws", "/login", "/docs/protocol/"]) {
  const response = await worker.fetch(
    new Request("https://world.forkmesh.com" + path), env
  );
  assert.equal(response.status, 308);
}
assert.deepEqual(delegated, [
  "https://world.forkmesh.com/api/world/ws",
  "https://world.forkmesh.com/login",
  "https://world.forkmesh.com/docs/protocol/",
]);
assert.deepEqual(assets, []);

await worker.fetch(new Request(
  "https://world.forkmesh.com/api-client.js?v=abc123"
), env);
await worker.fetch(new Request(
  "https://world.forkmesh.com/assets/logo.png"
), env);
assert.deepEqual(delegated.slice(3), [
  "https://app.forkmesh.com/api-client.js?v=abc123",
  "https://app.forkmesh.com/assets/logo.png",
]);
const shell = await worker.fetch(
  new Request("https://world.forkmesh.com/world"), env
);
assert.equal(shell.status, 200);
assert.equal(delegated.length, 5);
assert.deepEqual(assets, ["https://world.forkmesh.com/world/index.html"]);
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(worker_path)],
        cwd=REPOSITORY,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_world_mobile_health_checks_the_deployed_same_origin_boot_graph():
    worker_path = REPOSITORY / "world/worker.js"
    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
const fixtures = new Map([
  ["/world/index.html", ["text/html; charset=utf-8", 5190]],
  ["/world/world.js", ["text/javascript; charset=utf-8", 1283987]],
  ["/world/world-scene.js", ["text/javascript; charset=utf-8", 1407]],
  [
    "/world/vendor/three-0.184.0/three.module.min.js",
    ["text/javascript; charset=utf-8", 364998],
  ],
  [
    "/world/vendor/three-0.184.0/three.core.min.js",
    ["text/javascript; charset=utf-8", 384222],
  ],
]);
const requested = [];
let missing = "";
let appCalls = 0;
const env = {
  DEPLOY_FINGERPRINT: "mobile123",
  APP: { async fetch() { appCalls += 1; return new Response("app"); } },
  ASSETS: {
    async fetch(request) {
      const path = new URL(request.url).pathname;
      requested.push(path);
      assert.equal(request.method, "HEAD");
      assert.equal(request.headers.get("cache-control"), "no-cache");
      const [contentType, bytes] = fixtures.get(path) || ["text/plain", 0];
      return new Response(null, {
        status: path === missing || !fixtures.has(path) ? 404 : 200,
        headers: {
          "content-type": contentType,
          "content-length": String(bytes),
        },
      });
    },
  },
};
// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// harness routes the global fetch to the stand-in App.
globalThis.fetch = async (input, init) => env.APP.fetch(new Request(input, init));
const response = await worker.fetch(
  new Request("https://world.forkmesh.com/health/mobile"), env
);
assert.equal(response.status, 200);
assert.equal(response.headers.get("cache-control"), "no-store");
assert.equal(response.headers.get("x-forkmesh-worker"), "world");
assert.equal(
  response.headers.get("x-forkmesh-deploy-fingerprint"), "mobile123"
);
const payload = await response.json();
assert.equal(payload.ok, true);
assert.equal(payload.check, "mobile-world-startup");
assert.equal(payload.profile, "mobile");
assert.equal(payload.sameOriginRuntime, true);
assert.equal(payload.threeRevision, "184");
assert.deepEqual(payload.assets.map((item) => item.path), [...fixtures.keys()]);
assert.equal(payload.assets.every((item) => item.ok), true);
assert.deepEqual(requested, [...fixtures.keys()]);
assert.equal(appCalls, 0);

requested.length = 0;
missing = "/world/vendor/three-0.184.0/three.core.min.js";
const failed = await worker.fetch(
  new Request("https://world.forkmesh.com/health/mobile"), env
);
assert.equal(failed.status, 503);
const failedPayload = await failed.json();
assert.equal(failedPayload.ok, false);
assert.match(failedPayload.reason, /three\.core\.min\.js returned HTTP 404/);
assert.deepEqual(requested, [...fixtures.keys()]);

const rejected = await worker.fetch(
  new Request("https://world.forkmesh.com/health/mobile", { method: "POST" }),
  env
);
assert.equal(rejected.status, 405);
assert.equal(rejected.headers.get("allow"), "GET, HEAD");
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(worker_path)],
        cwd=REPOSITORY,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_www_proxies_legacy_api_posts_and_websocket_upgrades_to_app():
    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
const captured = [];
const env = {
  APP_ORIGIN: "https://app.forkmesh.com",
  WORLD_ORIGIN: "https://world.forkmesh.com",
  APP: {
    async fetch(request) {
      captured.push(request);
      return new Response("proxied", { status: 200 });
    },
  },
};
// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// harness routes the global fetch to the stand-in App.
globalThis.fetch = async (input, init) => env.APP.fetch(new Request(input, init));
const webhook = new Request("https://forkmesh.com/api/webhooks/mailtrap?event=1", {
  method: "POST",
  headers: { "content-type": "application/json", "x-signature": "signed" },
  body: '{"ok":true}',
});
assert.equal((await worker.fetch(webhook, env)).status, 200);
assert.equal(captured[0].url, "https://app.forkmesh.com/api/webhooks/mailtrap?event=1");
assert.equal(captured[0].method, "POST");
assert.equal(captured[0].headers.get("x-signature"), "signed");
assert.equal(await captured[0].text(), '{"ok":true}');
const upgrade = new Request("https://forkmesh.com/api/world/ws", {
  headers: { upgrade: "websocket", connection: "Upgrade" },
});
await worker.fetch(upgrade, env);
assert.equal(captured[1].url, "https://app.forkmesh.com/api/world/ws");
assert.equal(captured[1].headers.get("upgrade"), "websocket");
const landing = await worker.fetch(new Request("https://forkmesh.com/api"), env);
assert.equal(landing.status, 308);
assert.equal(landing.headers.get("location"), "https://app.forkmesh.com/api");
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(REPOSITORY / "www/worker.js")],
        cwd=REPOSITORY,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_www_feed_assets_bypass_the_app_feed_handler_without_redirect_loop():
    runtime = APP_RUNTIME.read_text(encoding="utf-8")
    route_start = runtime.index('if url.path in ("/api", "/api/"')
    feed_dispatch = runtime.index(
        "return await blog_rss_handler(self.env, request)", route_start)
    marketing_redirect = runtime.index("external_owner = external_site_route", route_start)
    assert feed_dispatch < marketing_redirect

    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
const captured = [];
const env = {
  APP_ORIGIN: "https://app.forkmesh.com",
  WORLD_ORIGIN: "https://world.forkmesh.com",
  APP: {
    async fetch() {
      throw new Error("WWW feeds must not reach App");
    },
  },
  ASSETS: {
    async fetch(request) {
      captured.push(request.url);
      return new Response("<rss/>", {
        status: 200,
        headers: { "content-type": "application/xml" },
      });
    },
  },
};
// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// harness routes the global fetch to the stand-in App.
globalThis.fetch = async (input, init) => env.APP.fetch(new Request(input, init));
for (const path of ["/blog/rss.xml", "/blog/feed.xml"]) {
  const response = await worker.fetch(new Request("https://forkmesh.com" + path), env);
  assert.equal(response.status, 200);
  assert.equal(response.headers.get("location"), null);
  assert.equal(response.headers.get("content-type"), "application/rss+xml; charset=utf-8");
}
assert.deepEqual(captured, [
  "https://forkmesh.com/rss.xml",
  "https://forkmesh.com/rss.xml",
]);
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(REPOSITORY / "www/worker.js")],
        cwd=REPOSITORY,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_www_static_prefixes_are_never_treated_as_repository_routes():
    harness = r'''
import fs from "node:fs";
import assert from "node:assert";
const source = fs.readFileSync(process.argv[1], "utf8");
const worker = (await import(
  "data:text/javascript;base64," + Buffer.from(source).toString("base64")
)).default;
let appCalls = 0;
const env = {
  APP_ORIGIN: "https://app.forkmesh.com",
  WORLD_ORIGIN: "https://world.forkmesh.com",
  APP: { async fetch() { appCalls += 1; return new Response("app"); } },
  ASSETS: { async fetch(request) { return new Response(new URL(request.url).pathname); } },
};
// Next has no App service binding: worker.js fetches APP_ORIGIN, so the
// harness routes the global fetch to the stand-in App.
globalThis.fetch = async (input, init) => env.APP.fetch(new Request(input, init));
for (const path of [
  "/assets/logo.png",
  "/assets/blog/features/one-app-one-mesh.webp",
  "/favicon/favicon.ico",
  "/blog/one-app-one-mesh/",
  "/docs/protocol/",
]) {
  const response = await worker.fetch(new Request("https://forkmesh.com" + path), env);
  assert.equal(await response.text(), path);
}
assert.equal(appCalls, 0);
'''
    completed = subprocess.run(
        ["node", "--input-type=module", "-e", harness, str(REPOSITORY / "www/worker.js")],
        cwd=REPOSITORY,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_deploy_script_documents_safe_cutover_order():
    deploy = (APP_ROOT / "deploy.sh").read_text(encoding="utf-8")
    assert "FORKMESH_PRESERVE_LEGACY_HOSTS=1" in deploy
    assert 'DEPLOY_VERIFY_URL="${DEPLOY_VERIFY_URL:-https://forkmesh.com}"' in deploy
    first_app = deploy.index(
        'FORKMESH_FORCE_DEPLOY=1 FORKMESH_PRESERVE_LEGACY_HOSTS=1 "$DEPLOY_SELF" app'
    )
    world = deploy.index('"$DEPLOY_SELF" world', first_app)
    www = deploy.index('"$DEPLOY_SELF" www', world)
    final_app = deploy.index('FORKMESH_FORCE_DEPLOY=1 "$DEPLOY_SELF" app', www)
    assert first_app < world < www < final_app
    assert "deploy_api_target" not in deploy
    assert "deploy_static_target app" not in deploy
    assert "api.next.forkmesh.com" not in deploy
    assert "forkmesh-api" not in deploy


def test_post_deploy_verifier_uses_an_explicit_operational_user_agent():
    verifier = (APP_ROOT / "tools/verify_split_deployment.py").read_text(
        encoding="utf-8"
    )
    assert '"User-Agent": "forkmesh-deploy-verify/1.0"' in verifier
