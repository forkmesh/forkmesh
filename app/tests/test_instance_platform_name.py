import ast
import re
import sys
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import apply_instance_brand  # noqa: E402
import instance_features  # noqa: E402
import og_card  # noqa: E402

DASHBOARD = ROOT / "public" / "dashboard"
JS = DASHBOARD / "js"
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
HOSTED_GIT = (ROOT / "src" / "hosted_git.py").read_text(encoding="utf-8")
BRAND_JS = (ROOT / "public" / "instance-brand.js").read_text(encoding="utf-8")

PLATFORM = re.compile(r"(?<![A-Za-z0-9_.$/-])ForkMesh(?![A-Za-z0-9_-])")

PLUGIN_FILES = {
    "09-ads.js": "ads",
    "09-hackathon.js": "hackathon",
    "09-links.js": "referrals",
    "09-referrals.js": "referrals",
}

KEPT = {
    "02-helpers.js": ['|| "ForkMesh";', '? "" : "ForkMesh ";'],
    "09-plugin-menu.js": ["on the ForkMesh Market"],
    "04-account.js": [
        "linked to another ForkMesh user",
        "not for the ForkMesh campaign",
        "ForkMesh plans remain free during launch",
        '"a ForkMesh award"',
        "is a ForkMesh node",
        "the ForkMesh app running on a machine",
    ],
    "05-repo-list-explorer.js": [
        "only a ForkMesh node holding your key",
        "hosts no ForkMesh git node",
        "until a ForkMesh node hosts",
    ],
    "06-repo-content.js": [
        "ForkMesh/ForkMesh/mcp/connector.json",
        "carries every ForkMesh tool",
        "a checkout of ForkMesh itself",
        '"a ForkMesh node"',
    ],
    "07-repo-compose-branch.js": ["runs a ForkMesh version"],
    "08-repo-detail-network.js": ["other ForkMesh instances"],
    "09-workflows.js": ["an n8n or ForkMesh workflow file", "The ForkMesh desktop app (MainWindow::"],
}


def env(**values):
    return SimpleNamespace(**values)


def platform_literals():
    found = []
    for path in sorted(JS.glob("*.js")):
        if path.name in PLUGIN_FILES:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            if line.lstrip().startswith("//"):
                continue
            for match in PLATFORM.finditer(line):
                if not any(kept in line for kept in KEPT.get(path.name, ())):
                    found.append("%s:%d: %s" % (path.name, number,
                                               line[max(0, match.start() - 50):match.end() + 30].strip()))
    return found


def test_dashboard_js_names_the_platform_through_the_helper():
    assert platform_literals() == []


def test_allow_list_entries_still_exist():
    for name, phrases in KEPT.items():
        text = (JS / name).read_text(encoding="utf-8")
        for phrase in phrases:
            assert phrase in text, (name, phrase)
    features = set(instance_features.IDS)
    assert set(PLUGIN_FILES.values()) <= features


def test_platform_name_helper_reads_the_brand_script():
    helpers = (JS / "02-helpers.js").read_text(encoding="utf-8")
    helper = helpers.split("  function platformName() {", 1)[1].split("\n  }", 1)[0]
    assert 'querySelector?.("script[data-instance-brand]")' in helper
    assert "dataset?.instanceName" in helper
    assert '|| "ForkMesh"' in helper
    assert "function applyPlatformName(root = document)" in helpers
    assert "[data-platform-name]" in helpers
    assert "async function instancePluginEnabled(id)" in helpers
    assert "if (!dashboardOnInstance()) return true;" in helpers
    boot = (JS / "10-boot.js").read_text(encoding="utf-8")
    assert "applyPlatformName();" in boot.split("async function bootDashboard()", 1)[1]


def test_repository_traffic_heading_uses_the_instance_name():
    compose = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    assert "ForkMesh traffic" not in compose
    assert "${escapeHtml(platformName())} traffic" in compose
    built = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert "ForkMesh traffic" not in built
    assert "${escapeHtml(platformName())} traffic" in built


def test_switched_off_plugins_are_not_requested_or_shown():
    ads = (JS / "09-ads.js").read_text(encoding="utf-8")
    slot = ads.split("  function initAdSlot() {", 1)[1].split("\n  }", 1)[0]
    assert 'instancePluginEnabled("ads")' in slot
    explorer = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
    blog = explorer.split("  async function loadHomeBlogPosts() {", 1)[1].split("\n  }", 1)[0]
    assert 'instancePluginEnabled("blog")' in blog
    compose = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    proof = compose.split("  function renderRepoAvailabilityProof() {", 1)[1]
    assert "const offsiteDocs = !dashboardOnInstance();" in proof
    assert proof.count("${offsiteDocs ? `<a") == 2
    network = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")
    assert "const fediHandle = dashboardOnInstance()" in network
    content = (JS / "06-repo-content.js").read_text(encoding="utf-8")
    assert "dashboardOnInstance() && body.fediverse?.handle" in content
    partials = DASHBOARD / "partials"
    main = (partials / "main.html").read_text(encoding="utf-8")
    assert '<section data-profile-tips data-instance-plugin="rewards"' in main
    overview = (partials / "views" / "profile-overview.html").read_text(encoding="utf-8")
    assert 'data-profile-referrals-panel data-instance-plugin="referrals"' in overview
    assert '<span data-instance-plugin="website">© 2026 ForkMesh, Inc.</span>' in overview
    settings = (partials / "views" / "settings.html").read_text(encoding="utf-8")
    assert "data-patreon-connection data-instance-plugin=\"patreon\"" in settings
    assert "data-home-redirect-setting data-instance-plugin=\"website\"" in settings
    membership = settings.split("ForkMesh membership</h3>", 1)[0].rsplit("<div", 3)[1]
    assert 'data-instance-plugin="patreon"' in membership


def test_content_page_partials_mark_the_platform_name():
    partials = DASHBOARD / "partials"
    for relative in ("views/profile-overview.html", "views/repos.html", "views/chat.html", "main.html"):
        text = (partials / relative).read_text(encoding="utf-8")
        text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
        text = text.replace("<span data-platform-name>ForkMesh</span>", "")
        text = text.replace('data-platform-name>ForkMesh</span>', "")
        text = text.replace("<span data-desktop-brand>ForkMesh </span>", "")
        text = text.replace("© 2026 ForkMesh, Inc.", "")
        assert not PLATFORM.search(text), (relative, PLATFORM.search(text).group(0),
                                           text[PLATFORM.search(text).start() - 60:][:120])


def test_rename_keeps_forkmesh_com_and_technical_names():
    plain = env()
    text = "Welcome to ForkMesh. X-ForkMesh-Ref, ForkMesh Desktop, ForkMesh, Inc."
    assert instance_features.rename(plain, text) == text
    assert instance_features.display_name(plain) == "ForkMesh"
    branded = env(INSTANCE_NAME="  ACME   Bolt ")
    assert instance_features.display_name(branded) == "ACME Bolt"
    assert instance_features.rename(branded, text) == (
        "Welcome to ACME Bolt. X-ForkMesh-Ref, ForkMesh Desktop, ForkMesh, Inc.")
    assert instance_features.rename(branded, "forkmesh.com and ForkMesh's") == (
        "forkmesh.com and ACME Bolt's")
    assert instance_features.rename_html(env(INSTANCE_NAME="A&B"), "<title>ForkMesh</title>") == (
        "<title>A&amp;B</title>")


def test_repository_page_head_uses_the_instance_name():
    page = APP_RUNTIME.split("    async def _serve_repo_page(", 1)[1].split("\n    async def ", 1)[0]
    assert "site_name = _html_escape(\n                instance_features.display_name(self.env))" in page
    assert 'title = _html_escape("%s - " % shown) + site_name' in page
    assert '<meta property=\\"og:site_name\\" content=\\"%s\\">' in page
    assert 'content=\\"ForkMesh\\"' not in page
    assert '"<head>", "<head>" + short_metas' in page
    profile = APP_RUNTIME.split('"<title>@%s · %s</title>" % (', 1)
    assert len(profile) == 2 and "instance_features.display_name(self.env)" in profile[1][:200]
    assert '" on the fediverse · %s</title>" % _html_escape(' in APP_RUNTIME
    early = (DASHBOARD / "partials" / "repo-early-reads.html").read_text(encoding="utf-8")
    assert "meta[name=\"forkmesh-repo-short\"]" in early
    assert "parts.unshift(shortOwner);" in early


def test_repository_card_names_the_site():
    assert "info.get(\"site\") or \"ForkMesh\"" in (ROOT / "src" / "og_card.py").read_text(encoding="utf-8")
    assert '"site": instance_name(env),' in ENTRY
    png = og_card.render_repo_card({"owner": "acme", "repo": "tool", "site": "Acme Code"}, 0)
    assert png.startswith(b"\x89PNG")


def _email_namespace():
    tree = ast.parse(ENTRY, filename="entry.py")
    wanted = {"instance_name", "instance_branded", "instance_site_url", "_instance_brand_email"}
    body = [node for node in tree.body
            if isinstance(node, ast.FunctionDef) and node.name in wanted]
    assert len(body) == len(wanted)
    namespace = {
        "FORKMESH_SITE_URL": "https://forkmesh.com/",
        "instance_features": instance_features,
        "instance_logo_path": lambda env: "",
        "_html_escape": lambda value: (str(value).replace("&", "&amp;").replace("<", "&lt;")
                                       .replace(">", "&gt;").replace('"', "&quot;")),
    }
    exec(compile(ast.Module(body=body, type_ignores=[]), "entry.py", "exec"), namespace)
    return namespace


def test_branded_email_body_and_subject_name_the_instance():
    brand = _email_namespace()["_instance_brand_email"]
    text = "Welcome to ForkMesh, ada.\n\nOpen ForkMesh: https://forkmesh.com/"
    html = "<p>Welcome to ForkMesh, ada.</p></div></div></body></html>"
    same = brand(env(), text, html)
    assert same == (text, html)
    out_text, out_html = brand(env(INSTANCE_NAME="A&B Code",
                                   PUBLIC_BASE_URL="https://code.example.com"), text, html)
    assert "Welcome to A&B Code, ada." in out_text
    assert "Open A&B Code: https://code.example.com/" in out_text
    assert out_text.endswith("Powered by ForkMesh")
    assert "Welcome to A&amp;B Code, ada." in out_html
    assert ">ForkMesh</a></p>" in out_html
    send = ENTRY.split("async def _send_email(env, to_email,", 1)[1].split("\nMAILTRAP_WEBHOOK_EVENTS", 1)[0]
    assert send.index("subject = instance_features.rename(env, subject)") < send.index(
        "_instance_brand_email(env, text, html)")


def test_founder_feedback_mail_stays_on_forkmesh_com():
    feedback = ENTRY.split("async def _send_feedback_emails(env):", 1)[1]
    assert feedback.lstrip().startswith("if instance_branded(env):\n        return 0")


def test_hosting_limit_messages_name_the_instance():
    assert "import instance_features" in HOSTED_GIT
    assert HOSTED_GIT.count("instance_features.rename(") == 2


def test_brand_tool_renames_page_text_outside_scripts(tmp_path):
    page = tmp_path / "index.html"
    page.write_text(
        '<!doctype html><html><head><title>Log in · ForkMesh</title>\n'
        '<meta name="description" content="Log in to ForkMesh with your node identity." />\n'
        '<meta name="description" content="Repositories mirrored by ForkMesh desktop nodes." />\n'
        '<meta property="og:title" content="Join ForkMesh" />\n'
        '</head><body><p>ForkMesh</p></body></html>')
    apply_instance_brand.apply(tmp_path, "Acme & Co")
    text = page.read_text()
    assert 'content="Log in to Acme &amp; Co with your node identity."' in text
    assert 'content="Repositories mirrored by desktop nodes."' in text
    assert 'content="Join Acme &amp; Co"' in text
    assert "<p>Acme &amp; Co</p>" in text


def test_brand_script_keeps_the_company_name_and_identifiers():
    assert "(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)" in BRAND_JS


def test_api_error_messages_name_the_instance():
    import json as _json
    sys.path.insert(0, str(ROOT / "host"))
    from forkmesh_host.runtime import HostRuntime
    from forkmesh_host.workers import Response

    def answer(env_values, payload, status=429, content_type="application/json"):
        response = Response(_json.dumps(payload), status=status,
                            headers={"content-type": content_type})
        holder = SimpleNamespace(env=env(**env_values))
        return HostRuntime._instance_error_text(holder, response)

    payload = {"error": "hosted_quota_exceeded",
               "message": "This account already hosts 5 repositories on ForkMesh.",
               "owner": "forkmesh"}
    renamed = answer({"INSTANCE_NAME": "Acme Code"}, payload)
    body = _json.loads(renamed._body)
    assert body["message"] == "This account already hosts 5 repositories on Acme Code."
    assert body["owner"] == "forkmesh" and body["error"] == "hosted_quota_exceeded"
    assert _json.loads(answer({}, payload)._body) == payload
    assert _json.loads(answer({"INSTANCE_NAME": "Acme Code"}, payload, status=200)._body) == payload
    text = answer({"INSTANCE_NAME": "Acme Code"}, payload, content_type="text/plain")
    assert _json.loads(text._body) == payload


def test_brand_tool_leaves_scripts_styles_and_identifiers_alone(tmp_path):
    page = tmp_path / "index.html"
    page.write_text(
        '<!doctype html><html><head><title>ForkMesh</title>'
        '<style>body{font-family:"ForkMesh Dashboard Mono"}</style>'
        '<script>window.ForkMeshAPI = "ForkMesh";</script></head>'
        '<body data-x="X-ForkMesh-Ref"><a aria-label="ForkMesh home">ForkMesh</a>'
        '<p>Open the ForkMesh desktop app.</p><span>© 2026 ForkMesh, Inc.</span></body></html>')
    apply_instance_brand.apply(tmp_path, "Acme Code")
    text = page.read_text()
    assert '"ForkMesh Dashboard Mono"' in text
    assert 'window.ForkMeshAPI = "ForkMesh";' in text
    assert 'X-ForkMesh-Ref' in text
    assert 'aria-label="Acme Code home">Acme Code</a>' in text
    assert "<p>Open the desktop app.</p>" in text
    assert "© 2026 ForkMesh, Inc." in text


def test_outbound_user_agents_name_the_instance_once_it_is_known():
    saved = dict(instance_features._REMEMBERED)
    try:
        instance_features._REMEMBERED.update({"name": "", "origin": ""})
        instance_features.remember_instance(env())
        assert instance_features.user_agent("forkmesh-relay/1.0", "webhooks") == "forkmesh-relay/1.0"
        instance_features.remember_instance(env(INSTANCE_NAME="ACME Bolt",
                                       PUBLIC_BASE_URL="https://git.example.org/"))
        assert instance_features.user_agent("forkmesh-relay/1.0", "webhooks") == (
            "acme-bolt-webhooks/1.0 (+https://git.example.org)")
    finally:
        instance_features._REMEMBERED.update(saved)
    sources = {name: (ROOT / "src" / name).read_text(encoding="utf-8") for name in (
        "repository_webhooks.py", "workflows_runtime.py", "repository_provider_mirror.py",
        "fediverse_routes.py")}
    assert all("instance_features.user_agent(" in text for text in sources.values())
    assert 'instance_features.user_agent(\n            "ForkMesh-repository-metadata/1.0"' in ENTRY
    runtime = (ROOT / "host" / "forkmesh_host" / "runtime.py").read_text(encoding="utf-8")
    assert runtime.count("self._remember_instance()") == 2


def test_brand_tool_names_the_web_manifest_without_a_logo(tmp_path):
    import json as _json
    (tmp_path / "favicon").mkdir()
    manifest = tmp_path / "favicon" / "site.webmanifest"
    manifest.write_text(_json.dumps({"name": "ForkMesh", "short_name": "ForkMesh", "display": "standalone"}))
    (tmp_path / "index.html").write_text("<!doctype html><html><head><title>ForkMesh</title></head></html>")
    apply_instance_brand.apply(tmp_path, "Acme Code")
    app = _json.loads(manifest.read_text())
    assert app["name"] == app["short_name"] == "Acme Code" and app["display"] == "standalone"


GATED_PAGES = {
    "pitch-deck": "pitch-deck",
    "referral": "referrals",
    "mirror-payouts.html": "rewards",
    "start.html": "instance-launcher",
    "hackathon": "hackathon",
    "microjobs": "microjobs",
    "ledger.html": "rewards",
    "leaderboards.html": "website",
    "market-reviews.html": "website",
    "status.html": "status",
    "dashboard/docs/": "developer-docs",
    "dashboard/ads/": "ads",
    "dashboard/links/": "referrals",
    "dashboard/bounties/": "referrals",
    "dashboard/challenges/": "challenges",
}
PAGE_KEPT = (
    "forkmesh.com/install.sh",
    "forkmesh-profile=username",
    "demo@forkmesh.local",
    "forkmesh-demo",
)
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
        "source", "track", "wbr"}
SHOWN_ATTRIBUTES = {"title", "aria-label", "alt", "placeholder", "content"}


def _shown_text(page):
    from html.parser import HTMLParser

    class Shown(HTMLParser):
        def __init__(self):
            super().__init__(convert_charrefs=True)
            self.stack = []
            self.lines = []

        def hidden(self):
            return any(self.stack)

        def handle_starttag(self, tag, attrs):
            values = dict(attrs)
            skip = tag in ("script", "style", "template") or "data-instance-plugin" in values
            if not self.hidden() and not skip:
                self.lines.extend(str(value or "") for name, value in attrs
                                  if name in SHOWN_ATTRIBUTES)
            if tag not in VOID:
                self.stack.append(skip)

        def handle_endtag(self, tag):
            if tag not in VOID and self.stack:
                self.stack.pop()

        def handle_data(self, data):
            if not self.hidden():
                self.lines.append(data)

    parser = Shown()
    parser.feed(page)
    return "\n".join(parser.lines)


def test_branded_pages_show_no_platform_word(tmp_path):
    import shutil
    dist = tmp_path / "dist"
    shutil.copytree(ROOT / "public", dist, ignore=shutil.ignore_patterns("*.js", "*.css", "*.png",
                                                                         "*.webp", "*.svg", "*.zip"))
    apply_instance_brand.apply(dist, "Acme Code")
    assert set(GATED_PAGES.values()) <= set(instance_features.IDS)
    found = []
    for page in sorted(dist.rglob("*.html")):
        relative = page.relative_to(dist).as_posix()
        if (relative.split("/", 1)[0] in GATED_PAGES or relative.startswith("dashboard/partials/")
                or any(relative.startswith(prefix) for prefix in GATED_PAGES if prefix.endswith("/"))):
            continue
        if relative.startswith("dashboard/") and relative.endswith(("shell.html",)):
            continue
        for line in _shown_text(page.read_text(encoding="utf-8", errors="replace")).split("\n"):
            if re.search(r"forkmesh", line, re.I) and not any(kept in line for kept in PAGE_KEPT):
                found.append("%s: %s" % (relative, line.strip()[:120]))
    assert found == []


def test_instance_guide_pages_do_not_say_forkmesh():
    import instance_pages
    for path in ("/desktop", "/docs"):
        page = instance_pages.page_for(path, "Acme Code", "https://code.example.org", False)
        shown = _shown_text(page)
        for line in shown.split("\n"):
            assert "forkmesh" not in line.lower() or "forkmesh.com/install.sh" in line, (path, line)
        assert "Powered by" not in page
    assert "Powered by" in instance_pages.page_for("/docs", "Acme Code", "https://code.example.org")


def test_the_instance_git_node_is_shown_as_the_instance_in_commits():
    helpers = (JS / "02-helpers.js").read_text(encoding="utf-8")
    assert 'instanceGitNode = String(info?.gitNode || "").toLowerCase();' in helpers
    assert "return Boolean(value) && dashboardOnInstance() && value === instanceGitNode;" in helpers
    assert "void instanceInfo();" in (JS / "10-boot.js").read_text(encoding="utf-8")
    compose = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    assert compose.count("shownAccountName(commit.author)") == 3
    assert "${platformAccount(item.author) ? escapeHtml(platformName()) : userNameHtml(item.author)}" in compose
    content = (JS / "06-repo-content.js").read_text(encoding="utf-8")
    assert "const platform = platformAccount(contributor?.name);" in content
    assert "shownAccountName(commit.author)" in content
    assert '"gitNode": str(getattr(self.env, "INSTANCE_FLAGSHIP_NODE", "")' in APP_RUNTIME
