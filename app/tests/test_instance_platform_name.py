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

PLATFORM = re.compile(r"(?<![A-Za-z0-9_.$/-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)")

PLUGIN_FILES = {
    "09-ads.js": "ads",
    "09-hackathon.js": "hackathon",
    "09-links.js": "referrals",
    "09-referrals.js": "referrals",
}

KEPT = {
    "02-helpers.js": ['|| "ForkMesh";'],
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
    "09-workflows.js": ["an n8n or ForkMesh workflow file"],
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
    assert 'title = "%s/%s - %s" % (owner, repo, site_name)' in page
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


def test_brand_tool_renames_page_descriptions_but_keeps_the_desktop(tmp_path):
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
    assert 'content="Repositories mirrored by ForkMesh desktop nodes."' in text
    assert 'content="Join Acme &amp; Co"' in text
    assert "<p>ForkMesh</p>" in text


def test_brand_script_keeps_the_company_name_and_identifiers():
    assert "(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)" in BRAND_JS
