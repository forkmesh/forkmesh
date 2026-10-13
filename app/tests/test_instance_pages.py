import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import instance_pages  # noqa: E402

APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ORIGIN = "https://code.example.com"


def test_desktop_page_explains_the_download_and_connecting_to_the_instance():
    for path in ("/desktop", "/desktop/", "/download", "/downloads"):
        page = instance_pages.page_for(path, "Acme Code", ORIGIN)
        assert "<title>Desktop app - Acme Code</title>" in page
        assert "curl -fsSL https://forkmesh.com/install.sh | bash" in page
        assert 'href="https://forkmesh.com/desktop"' in page
        assert "Add relay&hellip;" in page
        assert "<code>https://code.example.com</code>" in page
        assert 'href="/docs"' in page
        assert "Powered by" in page


def test_docs_page_is_a_generic_git_guide_for_the_instance():
    page = instance_pages.page_for("/docs", "Acme Code", ORIGIN)
    assert "<h1>Using Acme Code with Git</h1>" in page
    assert "git clone https://code.example.com/<em>owner</em>/<em>repository</em>.git" in page
    assert "git remote add origin https://code.example.com/" in page
    for anchor in ("account", "repository", "clone", "push", "branches",
                   "forks", "pulls", "other-instances", "issues", "desktop"):
        assert 'id="%s"' % anchor in page
        assert 'href="#%s"' % anchor in page
    assert "Forks on other instances" in page
    assert "forkmesh.com/docs" not in page
    assert instance_pages.page_for("/docs/", "Acme Code", ORIGIN) == page


def test_pages_escape_the_instance_name_and_ignore_other_paths():
    page = instance_pages.page_for("/docs", 'Acme <b>"Code"</b>', ORIGIN)
    assert "<b>" not in page
    assert "Acme &lt;b&gt;&quot;Code&quot;&lt;/b&gt;" in page
    for path in ("/", "/dashboard", "/docs/x", "/desktopx", "/status"):
        assert instance_pages.page_for(path, "Acme Code", ORIGIN) == ""


def test_router_serves_instance_pages_before_the_website_routes():
    route = APP_RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    head = route.split("delegated = self._world_ingress_redirect", 1)[0]
    assert "if instance_branded(self.env):" in head
    assert "instance_page = self._instance_page(request, url)" in head
    helper = APP_RUNTIME.split("    def _instance_page(self, request, url):", 1)[1]
    helper = helper.split("    async def _route(", 1)[0]
    assert 'url.path.startswith("/docs/")' in helper
    assert '"location": "/docs"' in helper
    assert "instance_pages.page_for(" in helper


def test_a_missing_desktop_profile_sends_people_to_the_download_page():
    profile = APP_RUNTIME.split(
        "    async def _serve_profile_page(self, url, name, repositories=False):", 1)[1]
    profile = profile.split("        cache_key = ", 1)[0]
    assert 'not rec and name == "desktop" and not repositories' in profile
    assert '"location": "/desktop"' in profile
