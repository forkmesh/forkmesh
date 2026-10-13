import re
import sys
import tomllib
from pathlib import Path
from urllib.parse import urlsplit


WWW = Path(__file__).resolve().parents[1]
REPOSITORY = WWW.parent
PUBLIC = WWW / "public"
APP_TOOLS = REPOSITORY / "app" / "tools"
if str(APP_TOOLS) not in sys.path:
    sys.path.insert(0, str(APP_TOOLS))

import build_site_assets  # noqa: E402
import docs_site  # noqa: E402

LLMS = (PUBLIC / "llms.txt").read_text(encoding="utf-8")
LINK_RE = re.compile(r"^- \[([^\]]+)\]\((https://[^)\s]+)\)(?:: (.+))?$")
OTHER_HOSTS = {"app.forkmesh.com", "world.forkmesh.com"}


def link_lines():
    return [line for line in LLMS.splitlines() if line.startswith("- [")]


def test_llms_txt_follows_the_llmstxt_layout():
    lines = LLMS.splitlines()
    assert lines[0] == "# ForkMesh"
    assert lines[1] == ""
    assert lines[2].startswith("> ")
    headings = [line for line in lines if line.startswith("#")]
    assert headings[0] == "# ForkMesh"
    assert all(line.startswith("## ") for line in headings[1:])
    assert headings[-1] == "## Optional"
    assert LLMS.endswith("\n")


def test_every_link_line_is_a_titled_described_link():
    links = link_lines()
    assert links
    for line in links:
        match = LINK_RE.match(line)
        assert match, line
        assert match.group(3), line
    urls = [LINK_RE.match(line).group(2) for line in links]
    assert len(urls) == len(set(urls))


def staged_file(root, path):
    if path == "/":
        return root / "index.html"
    relative = path.lstrip("/")
    if path.endswith("/"):
        return root / relative / "index.html"
    for candidate in (root / f"{relative}.html", root / relative):
        if candidate.is_file():
            return candidate
    return root / f"{relative}.html"


def test_forkmesh_links_resolve_to_staged_pages(tmp_path):
    build_site_assets.copy_tree(PUBLIC, tmp_path)
    docs_site.stage_docs(tmp_path)
    redirected = build_site_assets.redirect_sources(tmp_path)
    for line in link_lines():
        url = LINK_RE.match(line).group(2)
        parts = urlsplit(url)
        if parts.netloc in OTHER_HOSTS:
            continue
        assert parts.netloc == "forkmesh.com", url
        assert parts.path not in redirected, url
        assert staged_file(tmp_path, parts.path).is_file(), url


def test_llms_txt_is_a_static_asset_on_every_www_config():
    for name in ("wrangler.toml", "wrangler.production.toml"):
        config = tomllib.loads((WWW / name).read_text(encoding="utf-8"))
        assert "/llms.txt" not in config["assets"]["run_worker_first"]
