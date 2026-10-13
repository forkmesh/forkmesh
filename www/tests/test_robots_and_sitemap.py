import sys
import tomllib
from pathlib import Path
from xml.etree import ElementTree


WWW = Path(__file__).resolve().parents[1]
REPOSITORY = WWW.parent
PUBLIC = WWW / "public"
APP_TOOLS = REPOSITORY / "app" / "tools"
if str(APP_TOOLS) not in sys.path:
    sys.path.insert(0, str(APP_TOOLS))

import build_site_assets  # noqa: E402

SITE = "https://forkmesh.com"
ROBOTS = (PUBLIC / "robots.txt").read_text(encoding="utf-8")
SITEMAP_NS = "{http://www.sitemaps.org/schemas/sitemap/0.9}"


def robots_lines():
    return [line.strip() for line in ROBOTS.splitlines()
            if line.strip() and not line.startswith("#")]


def test_robots_names_only_the_wildcard_agent_and_the_sitemap():
    lines = robots_lines()
    agents = [line.split(":", 1)[1].strip() for line in lines
              if line.lower().startswith("user-agent:")]
    assert agents == ["*"]
    assert "Allow: /" in lines
    assert "Disallow: /" not in lines
    assert f"Sitemap: {SITE}/sitemap.xml" in lines
    assert "#" not in ROBOTS


def test_robots_keeps_crawlers_out_of_api_and_personal_pages():
    lines = robots_lines()
    for path in ("/api/", "/mcp", "/hackathon/join/", "/hackathon/squad/"):
        assert f"Disallow: {path}" in lines


def test_robots_and_sitemap_are_static_assets_on_every_www_config():
    for name in ("wrangler.toml", "wrangler.production.toml"):
        config = tomllib.loads((WWW / name).read_text(encoding="utf-8"))
        worker_first = config["assets"]["run_worker_first"]
        assert "/robots.txt" not in worker_first
        assert "/sitemap.xml" not in worker_first


def test_page_paths_follow_the_asset_router():
    page = build_site_assets.page_path
    assert page(Path("index.html")) == "/"
    assert page(Path("about.html")) == "/about"
    assert page(Path("docs/index.html")) == "/docs/"
    assert page(Path("blog/coves/index.html")) == "/blog/coves/"
    assert page(Path("hackathon/rules.html")) == "/hackathon/rules"


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_sitemap_uses_canonicals_and_skips_noindex_redirects_and_offsite(
        tmp_path):
    write(tmp_path / "index.html",
          f'<link rel="canonical" href="{SITE}/" />')
    write(tmp_path / "about.html", "<title>About</title>")
    write(tmp_path / "404.html", "<title>Missing</title>")
    write(tmp_path / "ios-install.html", "<title>Test build</title>")
    write(tmp_path / "hidden.html",
          '<meta name="robots" content="noindex, follow" />')
    write(tmp_path / "moved.html", "<title>Moved</title>")
    write(tmp_path / "elsewhere.html",
          '<link rel="canonical" href="https://app.forkmesh.com/x" />')
    write(tmp_path / "docs/guide/index.html",
          f'<link rel="canonical" href="{SITE}/docs/guide/" />')
    write(tmp_path / "docs/index.html", "<title>Docs</title>")
    write(tmp_path / "gone/index.html", "<title>Gone</title>")
    write(tmp_path / "_redirects",
          "/moved /about 308\n/docs /docs/ 308\n/gone /about 308\n"
          "/docs/:slug /docs/:slug/ 308\n")
    urls = [url for url, _ in build_site_assets.sitemap_urls(tmp_path)]
    assert urls == [f"{SITE}/", f"{SITE}/about", f"{SITE}/docs/",
                    f"{SITE}/docs/guide/"]


def test_sitemap_dates_blog_posts_from_the_blog_index(tmp_path):
    card = (PUBLIC / "blog.html").read_text(encoding="utf-8")
    write(tmp_path / "blog.html", card)
    entries = build_site_assets.blog_feed.parse_blog_index(card)
    dated = [entry for entry in entries if entry.get("publishedMs")]
    assert dated
    first = dated[0]
    slug_dir = first["url"][len(SITE) + 1:].rstrip("/")
    write(tmp_path / slug_dir / "index.html",
          f'<link rel="canonical" href="{first["url"].rstrip("/")}/">')
    urls = dict(build_site_assets.sitemap_urls(tmp_path))
    assert urls[first["url"].rstrip("/") + "/"]


def test_built_sitemap_covers_the_public_pages(tmp_path):
    build_site_assets.copy_tree(PUBLIC, tmp_path)
    build_site_assets.build_www_sitemap(tmp_path)
    root = ElementTree.parse(tmp_path / "sitemap.xml").getroot()
    assert root.tag == SITEMAP_NS + "urlset"
    urls = [node.text for node in root.iter(SITEMAP_NS + "loc")]
    assert urls[0] == f"{SITE}/"
    assert len(urls) == len(set(urls))
    for path in ("/about", "/blog", "/desktop", "/features", "/mobile",
                 "/docs/"):
        assert f"{SITE}{path}" in urls
    assert any(url.startswith(f"{SITE}/blog/") for url in urls)
    assert f"{SITE}/404" not in urls
    assert f"{SITE}/ios-install" not in urls
    redirected = build_site_assets.redirect_sources(tmp_path)
    for url in urls:
        assert url.startswith(f"{SITE}/")
        path = url[len(SITE):]
        assert path not in redirected
        assert redirected.get(path.rstrip("/") or "/", path) == path


def test_canonical_links_are_stamped_only_where_missing(tmp_path):
    write(tmp_path / "about.html",
          "<html>\n  <head>\n    <title>About</title>\n  </head>\n</html>\n")
    write(tmp_path / "docs/guide/index.html",
          "<head><title>Guide</title></head>")
    write(tmp_path / "kept.html",
          '<head><link rel="canonical" href="https://forkmesh.com/x" /></head>')
    write(tmp_path / "hidden.html",
          '<head><meta name="robots" content="noindex" /></head>')
    write(tmp_path / "404.html", "<head><title>Missing</title></head>")
    build_site_assets.stamp_canonical_links(tmp_path)
    about = (tmp_path / "about.html").read_text(encoding="utf-8")
    assert about == (
        "<html>\n  <head>\n    <title>About</title>\n"
        f'    <link rel="canonical" href="{SITE}/about" />\n'
        "  </head>\n</html>\n")
    guide = (tmp_path / "docs/guide/index.html").read_text(encoding="utf-8")
    assert f'<link rel="canonical" href="{SITE}/docs/guide/" />' in guide
    assert guide.index("canonical") < guide.index("</head>")
    for name in ("kept.html", "hidden.html", "404.html"):
        assert (tmp_path / name).read_text(encoding="utf-8").count(
            'rel="canonical"') == (1 if name == "kept.html" else 0)


def test_public_data_pages_are_staged_canonical_and_in_the_sitemap(tmp_path):
    build_site_assets.copy_tree(PUBLIC, tmp_path)
    app_public = REPOSITORY / "app" / "public"
    for name in build_site_assets.WWW_APP_PAGES:
        build_site_assets.copy_file(app_public / name, tmp_path / name)
    build_site_assets.stamp_canonical_links(tmp_path)
    build_site_assets.build_www_sitemap(tmp_path)
    urls = {node.text for node in ElementTree.parse(
        tmp_path / "sitemap.xml").getroot().iter(SITEMAP_NS + "loc")}
    redirected = build_site_assets.redirect_sources(tmp_path)
    for page in ("leaderboards", "network", "status"):
        html = (tmp_path / (page + ".html")).read_text(encoding="utf-8")
        assert f'<link rel="canonical" href="{SITE}/{page}" />' in html
        assert f"{SITE}/{page}" in urls
        assert "/" + page not in redirected
    assert (tmp_path / "leaderboards.js").is_file()
    unstamped = [
        path.relative_to(tmp_path).as_posix()
        for path in tmp_path.rglob("*.html")
        if 'rel="canonical"' not in path.read_text(encoding="utf-8")]
    assert sorted(unstamped) == ["404.html", "ios-install.html"]
