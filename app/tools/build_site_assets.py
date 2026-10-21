#!/usr/bin/env python3

from __future__ import annotations

import gzip
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import unquote, urlsplit
from xml.sax.saxutils import escape


REPOSITORY = Path(__file__).resolve().parents[2]
APP = REPOSITORY / "app"
WWW = REPOSITORY / "www"
WORLD = REPOSITORY / "world"
WORLD_TOOLS = WORLD / "tools"
OFFICE = REPOSITORY / "office"
MARKETPLACE = REPOSITORY / "marketplace"
MARKETPLACE_TOOLS = MARKETPLACE / "tools"
APP_SRC = APP / "src"
for import_root in (APP_SRC, MARKETPLACE_TOOLS, REPOSITORY / "tools"):
    if str(import_root) not in sys.path:
        sys.path.insert(0, str(import_root))

import blog_feed  # noqa: E402
import forkmesh_safe_delete  # noqa: E402
import docs_site  # noqa: E402
import dashboard_shell  # noqa: E402
import build_app_plugins  # noqa: E402
import build_dashboard_assets  # noqa: E402
import precompress_assets  # noqa: E402
import site_templates  # noqa: E402
from marketplace_lib import load_catalog, manifest_entries  # noqa: E402

SHARED_FILES = (
    "404.html",
    "api-client.js",
    "auth-mesh.css",
    "auth-mesh.js",
    "auth-page-theme.css",
    # Node-origin Blog documents are relayed rather than copied wholesale;
    # these small reader assets keep feature posts complete on that origin.
    "blog-social.css",
    "blog-social.js",
    "feature-post.css",
    "home-header-auth.js",
    "home-theme.js",
    "homepage-downloads.js",
    "mesh-page-theme.css",
    "noisy-background.avif",
    "analytics.js",
    "analytics-sdk.js",
    "genie-spider.js",
    "site-footer.css",
    "site-footer.js",
    "site-header.css",
    "site-header.js",
    "static-page.js",
    "styles.css",
    "tailwind.css",
)

WWW_APP_MODULES = (
    "membership-prompt.js",
)

WWW_APP_PAGES = (
    "leaderboards.html",
    "leaderboards.js",
    "network.html",
    "status.html",
)

BACKEND_ASSETS = (
    "blog.html",
    "index.html",
    "install.sh",
    "uninstall.sh",
)

WWW_FEED_ASSETS = (
    "rss.xml",
    "feed.xml",
    "blog/rss.xml",
    "blog/feed.xml",
)

WORLD_MODULE_SOURCES = {
    "world/world-runtime.js": "world/public/world/world.js",
    "world/world.js": "world/public/world/world-loader.js",
}
WORLD_MODULE_URL_RE = re.compile(
    r"""(["'`])((?:\.{1,2}/|/)[A-Za-z0-9_./-]+?\.js)((?:\?[^"'`\s]*)?)\1"""
)
SOURCE_MAP_COMMENT_RE = re.compile(r"\n?//# sourceMappingURL=[^\n]*\n?$")
OFFICE_FEATURE_CSS = (
    "identity", "presence", "work", "agents", "meetings", "whiteboard",
    "chat", "play", "profile", "rooms", "cheer", "focus", "campfire",
)
OFFICE_ENTRY_TAGS = (
    '<link rel="stylesheet" href="/office/style.css">',
    '<script type="module" src="/office/app.js"></script>',
)
WORLD_BUNDLE_ENTRIES = ("world-scene.js", "scene/quality.js", "scene/environment.js", "scene/resources.js")
WORLD_BUNDLE_SHARED_IMPORT_RE = re.compile(r"""\bfrom\s*["'](/world/[A-Za-z0-9_./-]+\.js)["']""")
WORLD_SCENE_PRELOAD_RE = re.compile(r'  "/world/scene/[A-Za-z0-9_./-]+\.js",\n')
WORLD_STATIC_IMPORT_RE = re.compile(
    r"""(?:\bfrom\s*|\bimport\s*)["']\./([A-Za-z0-9_-]+\.js)\?v=([0-9a-f]{12})["']"""
)

SITEMAP_ASSET = "sitemap.xml"
SITEMAP_EXCLUDED = frozenset({"404.html", "ios-install.html"})
CANONICAL_RE = re.compile(
    r'<link\s+rel="canonical"\s+href="([^"]+)"', re.IGNORECASE)
NOINDEX_RE = re.compile(
    r'<meta\s+name="robots"\s+content="[^"]*noindex', re.IGNORECASE)


def reset(path: Path) -> None:
    forkmesh_safe_delete.remove_tree(path, path.parent, ignore_errors=True)
    path.mkdir(parents=True)


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def copy_tree(source: Path, destination: Path) -> None:
    if destination.exists():
        forkmesh_safe_delete.remove_tree(destination, destination.parent)
    shutil.copytree(source, destination)


def shared_assets(destination: Path, *, include_video: bool = False) -> None:
    for name in SHARED_FILES:
        copy_file(WWW / "public" / name, destination / name)
    copy_tree(WWW / "public" / "favicon", destination / "favicon")
    asset_source = WWW / "public" / "assets"
    asset_destination = destination / "assets"
    asset_destination.mkdir(parents=True, exist_ok=True)
    for path in sorted(asset_source.iterdir()):
        if path.name == "blog" or (path.name == "video" and not include_video):
            continue
        target = asset_destination / path.name
        if path.is_dir():
            copy_tree(path, target)
        else:
            copy_file(path, target)


def world_marketplace_packages(destination: Path) -> None:
    marketplace_root = destination / "marketplace"
    copy_file(
        MARKETPLACE / "manifest.schema.json",
        marketplace_root / "manifest.schema.json",
    )
    package_root = marketplace_root / "packages"
    package_root.mkdir(parents=True, exist_ok=True)
    catalog = load_catalog(MARKETPLACE / "catalog.json")
    for entry, manifest in manifest_entries(catalog):
        if not (manifest.get("entrypoints") or {}).get("world"):
            continue
        source = (catalog.root / entry.manifest).parent
        # A package may keep its own test suite beside its content; tests
        # never ship as public World assets.
        staged = package_root / manifest["id"]
        if staged.exists():
            forkmesh_safe_delete.remove_tree(staged, package_root)
        shutil.copytree(
            source,
            staged,
            ignore=shutil.ignore_patterns("tests", "__pycache__"),
        )


def write_headers(destination: Path, worker: str) -> None:
    source = (WWW / "public" / "_headers").read_text(encoding="utf-8")
    source += f"\n/*\n  x-forkmesh-worker: {worker}\n"
    (destination / "_headers").write_text(source, encoding="utf-8")


def build_www_feeds(destination: Path) -> None:
    entries = blog_feed.parse_blog_index(
        (destination / blog_feed.BLOG_INDEX_ASSET).read_text(encoding="utf-8")
    )
    if not entries:
        raise RuntimeError("www blog index produced no RSS entries")
    for entry in entries:
        image = str(entry.get("image") or "")
        if not image:
            continue
        parsed = urlsplit(image)
        relative = Path(unquote(parsed.path).lstrip("/"))
        if not relative.parts or ".." in relative.parts:
            raise RuntimeError(f"invalid blog image path: {image}")
        image_path = destination / relative
        image_bytes = image_path.stat().st_size
        if image_bytes <= 0:
            raise RuntimeError(f"empty blog image: {relative.as_posix()}")
        entry["imageBytes"] = image_bytes
    built_ms = max(int(entry.get("publishedMs") or 0) for entry in entries) or 1
    document = blog_feed.render_rss(entries, built_ms=built_ms)
    encoded = document.encode("utf-8")
    for relative in WWW_FEED_ASSETS:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(encoded)


def redirect_sources(destination: Path) -> dict[str, str]:
    sources = {}
    redirects = destination / "_redirects"
    if not redirects.is_file():
        return sources
    for raw_line in redirects.read_text(encoding="utf-8").splitlines():
        parts = raw_line.split()
        if len(parts) >= 3 and not parts[0].startswith("#") and \
                parts[2].startswith("3") and ":" not in parts[0] and \
                "*" not in parts[0]:
            sources[parts[0]] = parts[1]
    return sources


def page_path(relative: Path) -> str:
    parts = relative.parts
    if parts[-1] == "index.html":
        return "/" + "".join(part + "/" for part in parts[:-1])
    return "/" + "/".join(parts[:-1] + (relative.stem,))


def sitemap_urls(destination: Path) -> list[tuple[str, str]]:
    site = blog_feed.SITE_URL
    lastmod = {}
    blog_index = destination / blog_feed.BLOG_INDEX_ASSET
    if blog_index.is_file():
        for entry in blog_feed.parse_blog_index(
                blog_index.read_text(encoding="utf-8")):
            stamp = int(entry.get("publishedMs") or 0)
            if stamp > 0 and entry.get("url"):
                day = time.strftime("%Y-%m-%d", time.gmtime(stamp / 1000))
                lastmod[str(entry["url"]).rstrip("/") + "/"] = day
    redirected = redirect_sources(destination)
    urls = {}
    for path in sorted(destination.rglob("*.html")):
        relative = path.relative_to(destination)
        if relative.as_posix() in SITEMAP_EXCLUDED:
            continue
        source = path.read_text(encoding="utf-8")
        if NOINDEX_RE.search(source):
            continue
        canonical = CANONICAL_RE.search(source)
        url = canonical.group(1) if canonical else site + page_path(relative)
        if url != site + "/" and not url.startswith(site + "/"):
            continue
        location = urlsplit(url).path or "/"
        bare = location.rstrip("/") or "/"
        if location in redirected or redirected.get(bare, location) != location:
            continue
        urls[url] = lastmod.get(url.rstrip("/") + "/", "")
    return sorted(urls.items(), key=lambda item: (item[0] != site + "/", item[0]))


def stamp_canonical_links(destination: Path) -> None:
    site = blog_feed.SITE_URL
    for path in sorted(destination.rglob("*.html")):
        relative = path.relative_to(destination)
        if relative.as_posix() in SITEMAP_EXCLUDED:
            continue
        source = path.read_text(encoding="utf-8")
        if CANONICAL_RE.search(source) or NOINDEX_RE.search(source):
            continue
        head_end = source.find("</head>")
        if head_end < 0:
            continue
        line_start = source.rfind("\n", 0, head_end) + 1
        indent = source[line_start:head_end]
        if indent.strip():
            indent = ""
        link = '%s  <link rel="canonical" href="%s" />\n' % (
            indent, escape(site + page_path(relative), {'"': "&quot;"}))
        path.write_text(
            source[:line_start] + link + source[line_start:], encoding="utf-8")


def build_www_sitemap(destination: Path) -> None:
    urls = sitemap_urls(destination)
    if not urls:
        raise RuntimeError("www sitemap produced no URLs")
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">']
    for url, modified in urls:
        entry = "  <url><loc>%s</loc>" % escape(url)
        if modified:
            entry += "<lastmod>%s</lastmod>" % modified
        lines.append(entry + "</url>")
    lines.append("</urlset>")
    (destination / SITEMAP_ASSET).write_text(
        "\n".join(lines) + "\n", encoding="utf-8")


def stamp_site_asset_versions(
    destination: Path, external_versions: dict[str, str] | None = None
) -> None:
    """Add content-derived versions only to deploy output, never source."""
    versions = dashboard_shell.staged_asset_versions(destination)
    versions.update(external_versions or {})
    for path in sorted(destination.rglob("*.html")):
        source = path.read_text(encoding="utf-8")
        path.write_text(
            dashboard_shell.stamp_asset_versions(source, versions),
            encoding="utf-8",
        )
    for path in sorted(destination.rglob("*.js")):
        if path.name in versions:
            continue
        source = path.read_text(encoding="utf-8")
        path.write_text(
            dashboard_shell.stamp_js_asset_versions(source, versions),
            encoding="utf-8",
        )


def finish_stage(
    destination: Path,
    worker: str,
    external_versions: dict[str, str] | None = None,
) -> Path:
    stamp_site_asset_versions(destination, external_versions)
    write_headers(destination, worker)
    return destination


MARKET_PREVIEW_SUFFIXES = {".png", ".svg", ".webp", ".jpg", ".jpeg"}


def stage_market_previews(destination: Path) -> None:
    for preview in sorted((MARKETPLACE / "packages").glob("*/preview/*")):
        if not preview.is_file() or preview.suffix.lower() not in MARKET_PREVIEW_SUFFIXES:
            continue
        relative = preview.relative_to(MARKETPLACE)
        copy_file(preview, destination / "market" / "previews" / relative)


def stage_www() -> Path:
    stale = site_templates.stale_pages()
    if stale:
        raise SystemExit(
            "www/public is out of date with www/templates (%s); run python3 app/tools/site_templates.py"
            % ", ".join(path.relative_to(WWW / "public").as_posix() for path in stale))
    destination = WWW / "dist"
    reset(destination)
    copy_tree(WWW / "public", destination)
    for name in WWW_APP_MODULES + WWW_APP_PAGES:
        copy_file(APP / "public" / name, destination / name)
    build_www_feeds(destination)
    stage_market_previews(destination)
    build_app_plugins.stage_www_pages(destination)
    docs_site.stage_docs(destination)
    stamp_canonical_links(destination)
    build_www_sitemap(destination)
    return finish_stage(destination, "www")


def stage_app() -> Path:
    destination = APP / "dist"
    reset(destination)
    copy_tree(APP / "public", destination)
    shared_assets(destination, include_video=True)
    for name in BACKEND_ASSETS:
        copy_file(WWW / "public" / name, destination / name)
    # The dependency-free QR generator the World already ships. The hackathon
    # share kit loads it on demand from this origin (script-src 'self'), so it
    # is staged here rather than duplicated into app/public.
    copy_file(WORLD / "public" / "qr.js", destination / "qr.js")
    build_app_plugins.stage_web(destination)
    build_dashboard_assets.minify_staged(destination, esbuild_binary())
    finish_stage(destination, "app")
    stage_office(destination)
    return destination


def office_budget() -> dict:
    return json.loads((OFFICE / "perf-budget.json").read_text(encoding="utf-8"))


def stage_office(destination: Path) -> Path:
    out = destination / "office"
    if out.exists():
        forkmesh_safe_delete.remove_tree(out, destination)
    out.mkdir(parents=True)
    public = OFFICE / "public"
    with tempfile.TemporaryDirectory(prefix=".office-", dir=destination.parent) as tmp:
        tmp_path = Path(tmp)
        css = [(public / "style.css").read_text(encoding="utf-8")]
        for name in OFFICE_FEATURE_CSS:
            css.append((public / "features" / (name + ".css")).read_text(encoding="utf-8"))
        (tmp_path / "office.css").write_text("\n".join(css), encoding="utf-8")
        common = ["--minify", "--charset=utf8", "--legal-comments=none", "--log-level=error"]
        subprocess.run(
            [esbuild_binary(), str(public / "app.js"), "--bundle", "--format=esm",
             "--target=es2022", "--outfile=" + str(tmp_path / "app.js"), *common],
            check=True, capture_output=True,
        )
        subprocess.run(
            [esbuild_binary(), str(tmp_path / "office.css"),
             "--outfile=" + str(tmp_path / "office.min.css"), *common],
            check=True, capture_output=True,
        )
        script = (tmp_path / "app.js").read_text(encoding="utf-8")
        style = (tmp_path / "office.min.css").read_text(encoding="utf-8")
    budget = office_budget()
    script_gz = len(gzip.compress(script.encode("utf-8"), 9))
    style_gz = len(gzip.compress(style.encode("utf-8"), 9))
    if script_gz > budget["scriptGzipBytes"] or style_gz > budget["styleGzipBytes"]:
        raise RuntimeError(
            "office bundle over budget: script %d/%d, style %d/%d gzip bytes" % (
                script_gz, budget["scriptGzipBytes"], style_gz, budget["styleGzipBytes"]))
    if "eval(" in script or "new Function" in script:
        raise RuntimeError("office bundle must not evaluate strings")
    script_version = dashboard_shell.content_version(script)
    style_version = dashboard_shell.content_version(style)
    logo = (public / "logo.svg").read_text(encoding="utf-8")
    logo_version = dashboard_shell.content_version(logo)
    (out / "app.js").write_text(script, encoding="utf-8")
    (out / "office.css").write_text(style, encoding="utf-8")
    (out / "logo.svg").write_text(logo, encoding="utf-8")
    index = (public / "index.html").read_text(encoding="utf-8")
    for tag in OFFICE_ENTRY_TAGS:
        if tag not in index:
            raise RuntimeError("office/public/index.html is missing " + tag)
    index = index.replace(OFFICE_ENTRY_TAGS[0], '<link rel="stylesheet" href="/office/office.css?v=%s">' % style_version)
    index = index.replace(OFFICE_ENTRY_TAGS[1], '<script type="module" src="/office/app.js?v=%s"></script>' % script_version)
    index = index.replace('/office/logo.svg"', '/office/logo.svg?v=%s"' % logo_version)
    (out / "index.html").write_text(index, encoding="utf-8")
    (out / "size.json").write_text(json.dumps({
        "scriptBytes": len(script.encode("utf-8")), "scriptGzipBytes": script_gz,
        "styleBytes": len(style.encode("utf-8")), "styleGzipBytes": style_gz,
    }, sort_keys=True) + "\n", encoding="utf-8")
    return out


def stage_cutover() -> Path:
    destination = APP / "dist"
    reset(destination)
    copy_tree(APP / "public", destination)
    shared_assets(destination, include_video=True)
    for name in BACKEND_ASSETS:
        copy_file(WWW / "public" / name, destination / name)
    shutil.copytree(WWW / "public", destination, dirs_exist_ok=True)
    shutil.copytree(WORLD / "public", destination, dirs_exist_ok=True)
    docs_site.stage_docs(destination)
    world_marketplace_packages(destination)
    build_app_plugins.stage_web(destination)
    build_dashboard_assets.minify_staged(destination, esbuild_binary())
    return finish_stage(destination, "app")


def stage_world() -> Path:
    destination = WORLD / "dist"
    reset(destination)
    copy_tree(WORLD / "public", destination)
    # Keep the large authored World implementation easy to test in place, but
    # deploy a first-paint shell at its stable public URL. The shell crosses a
    # dynamic import boundary only after one animation frame; the runtime and
    # all of its static imports therefore stay outside the initial graph.
    world_directory = destination / "world"
    copy_file(
        world_directory / "world.js",
        world_directory / "world-runtime.js",
    )
    copy_file(
        world_directory / "world-loader.js",
        world_directory / "world.js",
    )
    (world_directory / "world-loader.js").unlink()
    world_marketplace_packages(destination)
    # App-owned dependencies stay out of the World upload. world/worker.js
    # service-proxies them from the App while retaining same-origin browser
    # URLs, CSP, and relative ES-module imports.
    external_versions = {
        "api-client.js": dashboard_shell.content_version(
            (WWW / "public" / "api-client.js").read_text(encoding="utf-8")
        ),
        "dashboard-chat.js": dashboard_shell.content_version(
            (APP / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
        ),
        "membership-prompt.js": dashboard_shell.content_version(
            (APP / "public" / "membership-prompt.js").read_text(encoding="utf-8")
        ),
    }
    finish_stage(destination, "world", external_versions)

    bundled = []
    if os.environ.get("FORKMESH_WORLD_BUNDLE", "1") != "0":
        bundled = bundle_world_scene(destination)
    files = world_module_files(destination)
    if os.environ.get("FORKMESH_WORLD_MINIFY", "1") != "0":
        minify_world_modules(destination, [name for name in files if name not in bundled])
    versions = version_world_modules(destination, files)
    runtime_version = versions["world/world-runtime.js"]
    loader_version = versions["world/world.js"]
    loader = (world_directory / "world.js").read_text(encoding="utf-8")
    if f"./world-runtime.js?v={runtime_version}" not in loader:
        raise RuntimeError("World loader is missing its runtime import")
    index_path = world_directory / "index.html"
    index = index_path.read_text(encoding="utf-8")
    entry_tag = '<script type="module" src="/world/world.js"></script>'
    if entry_tag not in index:
        raise RuntimeError("World document is missing its entry script tag")
    runtime_text = (world_directory / "world-runtime.js").read_text(encoding="utf-8")
    runtime_imports = []
    for name, version in WORLD_STATIC_IMPORT_RE.findall(runtime_text):
        if (name, version) not in runtime_imports:
            runtime_imports.append((name, version))
    preloads = [f"/world/world-runtime.js?v={runtime_version}"] + [
        f"/world/{name}?v={version}" for name, version in runtime_imports
    ]
    index = index.replace(
        entry_tag,
        "".join(
            f'<link rel="modulepreload" href="{href}" />\n    ' for href in preloads
        )
        + entry_tag,
        1,
    )
    index = rewrite_module_urls(index, "world/index.html", versions)
    if f'src="/world/world.js?v={loader_version}"' not in index:
        raise RuntimeError("World document entry was not versioned")
    index_path.write_text(index, encoding="utf-8")
    return destination


def node_tool(name: str) -> str | None:
    found = shutil.which(name)
    if found:
        return found
    for candidate in sorted(
        (Path.home() / ".nvm" / "versions" / "node").glob("v*/bin"),
        reverse=True,
    ):
        if (candidate / name).exists():
            return str(candidate / name)
    return None


def esbuild_binary() -> str:
    configured = os.environ.get("FORKMESH_ESBUILD", "")
    if configured:
        return configured
    binary = WORLD_TOOLS / "node_modules" / ".bin" / "esbuild"
    if not binary.exists():
        npm = node_tool("npm")
        if npm is None:
            raise RuntimeError("World build needs npm to install esbuild (world/tools)")
        env = dict(os.environ)
        env["PATH"] = str(Path(npm).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run(
            [npm, "ci", "--prefer-offline", "--no-audit", "--no-fund"],
            cwd=WORLD_TOOLS,
            env=env,
            check=True,
            capture_output=True,
        )
    if not binary.exists():
        raise RuntimeError("esbuild is missing from world/tools/node_modules")
    return str(binary)


def esbuild_package() -> Path:
    package = Path(os.path.realpath(esbuild_binary())).parent.parent
    if (package / "package.json").exists():
        return package
    fallback = WORLD_TOOLS / "node_modules" / "esbuild"
    if (fallback / "package.json").exists():
        return fallback
    raise RuntimeError("World bundle needs the esbuild package in world/tools/node_modules")


def bundled_map_source(map_directory: Path, source: str, world_directory: Path) -> str:
    resolved = Path(os.path.normpath(map_directory / source))
    try:
        return "world/public/world/" + resolved.relative_to(world_directory).as_posix()
    except ValueError:
        return source


def bundle_world_scene(destination: Path) -> list[str]:
    world_directory = destination / "world"
    node = node_tool("node")
    if node is None:
        raise RuntimeError("World bundle needs node")
    outputs = []
    with tempfile.TemporaryDirectory(prefix=".bundle-", dir=destination.parent) as tmp:
        subprocess.run(
            [
                node,
                str(WORLD_TOOLS / "bundle_world_scene.mjs"),
                str(esbuild_package()),
                str(world_directory),
                tmp,
                *WORLD_BUNDLE_ENTRIES,
            ],
            check=True,
        )
        forkmesh_safe_delete.remove_tree(world_directory / "scene", world_directory)
        for built in sorted(Path(tmp).rglob("*.js")):
            relative = "world/" + built.relative_to(tmp).as_posix()
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            text = SOURCE_MAP_COMMENT_RE.sub("", built.read_text(encoding="utf-8"))
            target.write_text(text + "\n", encoding="utf-8")
            source_map = json.loads(
                built.with_name(built.name + ".map").read_text(encoding="utf-8")
            )
            source_map["sources"] = [
                bundled_map_source(built.parent, source, world_directory)
                for source in source_map["sources"]
            ]
            source_map["sourceRoot"] = ""
            (destination / (relative + ".map")).write_text(
                json.dumps(source_map, separators=(",", ":")), encoding="utf-8"
            )
            outputs.append(relative)
    loader_path = world_directory / "world.js"
    loader = loader_path.read_text(encoding="utf-8")
    scene_preloads = WORLD_SCENE_PRELOAD_RE.findall(loader)
    if not scene_preloads:
        raise RuntimeError("World loader has no scene module preloads to replace")
    loader = WORLD_SCENE_PRELOAD_RE.sub("", loader)
    anchor = '  "/world/world-scene.js",\n'
    if anchor not in loader:
        raise RuntimeError("World loader is missing the world-scene.js preload")
    shared = []
    for name in outputs:
        text = (destination / name).read_text(encoding="utf-8")
        for href in WORLD_BUNDLE_SHARED_IMPORT_RE.findall(text):
            if href not in shared and href.startswith("/world/") and not href.startswith("/world/scene/"):
                shared.append(href)
    extra = "".join(
        f'  "/{name}",\n'
        for name in outputs
        if name != "world/world-scene.js"
    ) + "".join(f'  "{href}",\n' for href in shared)
    loader_path.write_text(loader.replace(anchor, anchor + extra, 1), encoding="utf-8")
    return outputs


def world_module_files(destination: Path) -> list[str]:
    files = []
    for path in sorted(destination.rglob("*.js")):
        relative = path.relative_to(destination).as_posix()
        if relative.startswith("world/vendor/"):
            continue
        if (
            relative.startswith("world/")
            or relative == "qr.js"
            or re.match(r"^marketplace/packages/[^/]+/content/world/", relative)
        ):
            files.append(relative)
    return files


def world_module_source(relative: str) -> str:
    if relative in WORLD_MODULE_SOURCES:
        return WORLD_MODULE_SOURCES[relative]
    if relative.startswith("marketplace/"):
        return relative
    return "world/public/" + relative


def minify_world_modules(destination: Path, files: list[str]) -> None:
    if not files:
        return
    with tempfile.TemporaryDirectory(prefix=".minify-", dir=destination.parent) as tmp:
        subprocess.run(
            [
                esbuild_binary(),
                *[str(destination / relative) for relative in files],
                "--minify",
                "--sourcemap=external",
                "--charset=utf8",
                "--legal-comments=none",
                "--log-level=error",
                f"--outbase={destination}",
                f"--outdir={tmp}",
            ],
            check=True,
            capture_output=True,
        )
        for relative in files:
            output = Path(tmp) / relative
            text = SOURCE_MAP_COMMENT_RE.sub(
                "", output.read_text(encoding="utf-8")
            )
            (destination / relative).write_text(text + "\n", encoding="utf-8")
            source_map = json.loads(
                output.with_name(output.name + ".map").read_text(encoding="utf-8")
            )
            source_map["sources"] = [world_module_source(relative)]
            source_map["sourceRoot"] = ""
            (destination / (relative + ".map")).write_text(
                json.dumps(source_map, separators=(",", ":")), encoding="utf-8"
            )


def resolve_module_url(owner: str, specifier: str) -> str:
    if specifier.startswith("/"):
        return posixpath.normpath(specifier).lstrip("/")
    base = posixpath.dirname("/" + owner)
    return posixpath.normpath(posixpath.join(base, specifier)).lstrip("/")


def versioned_module_url(specifier: str, query: str, version: str) -> str:
    if not query:
        return f"{specifier}?v={version}"
    return f"{specifier}?v={version}&{query[1:]}"


def rewrite_module_urls(text: str, owner: str, versions: dict[str, str]) -> str:
    def replace(match):
        quote, specifier, query = match.group(1), match.group(2), match.group(3)
        target = resolve_module_url(owner, specifier)
        if target not in versions or query.startswith("?v="):
            return match.group(0)
        return quote + versioned_module_url(specifier, query, versions[target]) + quote

    return WORLD_MODULE_URL_RE.sub(replace, text)


def world_module_order(destination: Path, files: list[str]) -> list[str]:
    known = set(files)
    edges = {}
    for relative in files:
        text = (destination / relative).read_text(encoding="utf-8")
        edges[relative] = sorted(
            {
                target
                for match in WORLD_MODULE_URL_RE.finditer(text)
                for target in [resolve_module_url(relative, match.group(2))]
                if target in known and target != relative
            }
        )
    order = []
    state = {}

    def visit(node, trail):
        if state.get(node) == "done":
            return
        if state.get(node) == "open":
            raise RuntimeError("World module import cycle: " + " -> ".join(trail + [node]))
        state[node] = "open"
        for target in edges[node]:
            visit(target, trail + [node])
        state[node] = "done"
        order.append(node)

    for relative in files:
        visit(relative, [])
    return order


def version_world_modules(destination: Path, files: list[str]) -> dict[str, str]:
    versions = {}
    for relative in world_module_order(destination, files):
        path = destination / relative
        text = rewrite_module_urls(path.read_text(encoding="utf-8"), relative, versions)
        if (destination / (relative + ".map")).exists():
            text = text.rstrip("\n") + f"\n//# sourceMappingURL={path.name}.map\n"
        path.write_text(text, encoding="utf-8")
        versions[relative] = dashboard_shell.content_version(text)
    return versions


def stage_single() -> Path:
    return stage_app()


WORLD_CANONICAL_RE = re.compile(r'[ \t]*<link rel="canonical" href="[^"]*" />\n?')


def stage_instance_world() -> Path:
    destination = APP / "dist"
    if not (destination / "index.html").is_file():
        raise RuntimeError("stage the app (build_site_assets.py app) before instance-world")
    world = stage_world()
    target = destination / "world"
    if target.exists():
        forkmesh_safe_delete.remove_tree(target, destination)
    for source in sorted(world.rglob("*")):
        relative = source.relative_to(world)
        if not source.is_file() or relative.as_posix() == "_headers":
            continue
        output = destination / relative
        if relative.parts[0] != "world" and output.exists():
            continue
        copy_file(source, output)
    index = target / "index.html"
    index.write_text(WORLD_CANONICAL_RE.sub("", index.read_text(encoding="utf-8"), count=1), encoding="utf-8")
    # The app's own files were compressed by the host build; this adds the
    # World's (precompress_assets skips copies that are still fresh).
    precompress_assets.precompress(destination)
    return target


def main(arguments: list[str]) -> int:
    stagers = {
        "app": stage_app,
        "cutover": stage_cutover,
        "single": stage_single,
        "world": stage_world,
        "www": stage_www,
    }
    targets = arguments[1:] or list(stagers)
    stagers["instance-world"] = stage_instance_world
    for target in targets:
        stager = stagers.get(target)
        if stager is None:
            print(f"unknown site target: {target}", file=sys.stderr)
            return 2
        destination = stager()
        files = sum(path.is_file() for path in destination.rglob("*"))
        print(f"staged {target}: {files} files in {destination.relative_to(REPOSITORY)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
