import base64
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import apply_instance_brand  # noqa: E402
import instance_features  # noqa: E402
import mail_providers  # noqa: E402

ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
BRAND_JS = (ROOT / "public" / "instance-brand.js").read_text(encoding="utf-8")

MESSAGE = {
    "sender": "no-reply@code.example.com",
    "senderName": "Acme Code",
    "to": "ada@example.com",
    "subject": "Welcome",
    "text": "Hello",
    "html": "<p>Hello</p>",
    "category": "Welcome verification",
    "variables": {"forkmesh_send_id": "s1", "forkmesh_kind": "verification"},
}


def env(**values):
    return SimpleNamespace(**values)


def test_provider_choice_respects_the_explicit_setting_and_its_key():
    assert mail_providers.provider(env()) == ""
    assert mail_providers.provider(env(MAILTRAP_API_TOKEN="t")) == "mailtrap"
    assert mail_providers.provider(env(), True) == "smtp"
    assert mail_providers.provider(env(MAIL_PROVIDER="sendgrid",
                                       SENDGRID_API_KEY="SG.x"), True) == "sendgrid"
    assert mail_providers.provider(env(MAIL_PROVIDER="none"), True) == ""
    assert mail_providers.provider(env(MAIL_PROVIDER="sendgrid")) == ""
    assert mail_providers.provider(env(MAIL_PROVIDER="mailgun", MAILGUN_API_KEY="k")) == ""
    assert mail_providers.provider(env(MAIL_PROVIDER="mailgun", MAILGUN_API_KEY="k",
                                       MAILGUN_DOMAIN="mg.example.com")) == "mailgun"
    assert mail_providers.provider(env(MAIL_PROVIDER="none",
                                       MAILTRAP_API_TOKEN="t")) == ""


def test_mailtrap_request_keeps_its_existing_shape():
    url, headers, body = mail_providers.build_request(
        env(MAILTRAP_API_TOKEN="tok"), "mailtrap", MESSAGE)
    assert url == "https://send.api.mailtrap.io/api/send"
    assert headers["authorization"] == "Bearer tok"
    payload = json.loads(body)
    assert payload["from"] == {"email": MESSAGE["sender"], "name": "Acme Code"}
    assert payload["to"] == [{"email": "ada@example.com"}]
    assert payload["category"] == "Welcome verification"
    assert payload["custom_variables"]["forkmesh_send_id"] == "s1"
    assert payload["html"] == "<p>Hello</p>"


def test_sendgrid_request():
    url, headers, body = mail_providers.build_request(
        env(SENDGRID_API_KEY="SG.key"), "sendgrid", MESSAGE)
    assert url == "https://api.sendgrid.com/v3/mail/send"
    assert headers == {"content-type": "application/json",
                       "authorization": "Bearer SG.key"}
    payload = json.loads(body)
    assert payload["personalizations"] == [{
        "to": [{"email": "ada@example.com"}],
        "custom_args": {"forkmesh_send_id": "s1", "forkmesh_kind": "verification"}}]
    assert payload["content"] == [{"type": "text/plain", "value": "Hello"},
                                  {"type": "text/html", "value": "<p>Hello</p>"}]
    assert payload["categories"] == ["Welcome verification"]
    assert mail_providers.message_id(
        "sendgrid", {"x-message-id": "abc"}, "") == "abc"


def test_mailgun_request_uses_the_domain_region_and_basic_auth():
    url, headers, body = mail_providers.build_request(
        env(MAILGUN_API_KEY="key-1", MAILGUN_DOMAIN="mg.example.com",
            MAILGUN_REGION="eu"), "mailgun", MESSAGE)
    assert url == "https://api.eu.mailgun.net/v3/mg.example.com/messages"
    assert headers["content-type"] == "application/x-www-form-urlencoded"
    assert base64.b64decode(headers["authorization"].split()[1]) == b"api:key-1"
    fields = parse_qs(body)
    assert fields["from"] == ['"Acme Code" <no-reply@code.example.com>']
    assert fields["to"] == ["ada@example.com"]
    assert fields["o:tag"] == ["Welcome verification"]
    assert fields["v:forkmesh_send_id"] == ["s1"]
    url, _, _ = mail_providers.build_request(
        env(MAILGUN_API_KEY="k", MAILGUN_DOMAIN="mg.example.com"), "mailgun", MESSAGE)
    assert url.startswith("https://api.mailgun.net/v3/")
    assert mail_providers.message_id(
        "mailgun", {}, '{"id": "<20260101.1@mg.example.com>"}') == "20260101.1@mg.example.com"
    assert mail_providers.message_id(
        "mailtrap", {}, '{"message_ids": ["m-1"]}') == "m-1"


def test_send_email_goes_through_the_provider_table():
    send = ENTRY.split("async def _send_email(env, to_email,", 1)[1].split(
        "\nMAILTRAP_WEBHOOK_EVENTS", 1)[0]
    assert "provider = mail_providers.provider(env, smtp_mail.enabled(env))" in send
    assert "mail_providers.build_request(" in send
    assert "_instance_brand_email(env, text, html)" in send
    assert '"Bearer " + token' not in send


def test_branded_email_names_the_instance_and_credits_forkmesh():
    helper = ENTRY.split("def _instance_brand_email(env, text, html):", 1)[1].split(
        "\n\n\n", 1)[0]
    assert "Powered by ForkMesh" in helper
    assert "data-instance-powered-by" in helper
    assert 'if not instance_branded(env):' in helper


def test_forkmesh_com_keeps_every_feature():
    plain = env()
    assert instance_features.enabled_ids(plain) == set(instance_features.IDS)
    for path in ("/api/hackathon", "/world", "/api/world/state", "/about"):
        assert instance_features.blocked(plain, path) == ""


def test_white_label_instances_switch_forkmesh_features_off():
    branded = env(INSTANCE_NAME="Acme Code")
    assert instance_features.enabled_ids(branded) == set(
        instance_features.DEFAULT_ON)
    assert instance_features.blocked(branded, "/api/hackathon/join") == "hackathon"
    assert instance_features.blocked(branded, "/world") == "world"
    assert instance_features.blocked(branded, "/api/orgs/acme/discord/oauth") == "discord"
    assert instance_features.blocked(branded, "/api/orgs/acme/members") == ""
    assert instance_features.blocked(branded, "/worldly") == ""
    assert instance_features.blocked(branded, "/dashboard/repos") == ""
    assert instance_features.blocked(branded, "/api/accounts/login") == ""
    assert instance_features.blocked(branded, "/acme/forkmesh") == ""
    chosen = env(INSTANCE_NAME="Acme Code", INSTANCE_PLUGINS="world, hackathon, bogus")
    assert instance_features.enabled_ids(chosen) == {
        "world", "hackathon"} | instance_features.DEFAULT_ON
    assert instance_features.blocked(chosen, "/world") == ""
    everything = env(INSTANCE_NAME="Acme Code", INSTANCE_PLUGINS="all")
    assert instance_features.blocked(everything, "/api/ads") == ""
    described = {item["id"]: item for item in instance_features.describe(chosen)}
    assert described["world"]["enabled"] is True
    assert described["ads"]["enabled"] is False
    assert "/api/ads" in described["ads"]["paths"]


def test_router_gates_before_any_route_and_publishes_the_instance():
    route = APP_RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    head = route.split("delegated = self._world_ingress_redirect", 1)[0]
    assert "instance_features.blocked(self.env, url.path)" in head
    assert '"/api/instance"' in head
    assert '"poweredBy"' in head
    assert "plugin_disabled" in APP_RUNTIME


def test_brand_tool_renames_titles_and_adds_the_script_once(tmp_path):
    page = tmp_path / "dashboard" / "index.html"
    page.parent.mkdir()
    page.write_text(
        "<!doctype html><html><head><title>Repos - ForkMesh</title>\n"
        '<meta name="application-name" content="ForkMesh" />\n'
        "  </head><body><h1>ForkMesh</h1></body></html>")
    assert apply_instance_brand.apply(tmp_path, "Acme Code") == 1
    text = page.read_text()
    assert "<title>Repos - Acme Code</title>" in text
    assert 'content="Acme Code"' in text
    assert text.count("/instance-brand.js") == 1
    assert 'data-instance-name="Acme Code"' in text
    assert (tmp_path / "instance-brand.js").read_text() == BRAND_JS
    assert apply_instance_brand.apply(tmp_path, "Acme Code") == 0
    assert apply_instance_brand.apply(tmp_path, "Beta Git") == 1
    assert page.read_text().count("/instance-brand.js") == 1
    assert 'data-instance-name="Beta Git"' in page.read_text()


def test_brand_script_credits_forkmesh_and_hides_switched_off_links():
    assert "Powered by " in BRAND_JS and "https://forkmesh.com/" in BRAND_JS
    assert "data-instance-powered-by" in BRAND_JS
    assert 'fetch("/api/instance"' in BRAND_JS
    assert "MutationObserver" in BRAND_JS
    assert "ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)" in BRAND_JS


def test_brand_script_leaves_repository_and_chat_content_alone():
    assert "[data-repo-detail]" in BRAND_JS
    assert "[data-repo-readme-body]" in BRAND_JS
    assert "[class*='commit']" in BRAND_JS
    assert "const CONTENT_PAGES = /^(repo|profile)/;" in BRAND_JS
    assert "CONTENT_PATHS = /^\\/(chat|notes|docs)(\\/|$)/" in BRAND_JS
    leave = BRAND_JS.split("const leaveAlone = (node) => {", 1)[1].split("\n  };", 1)[0]
    assert "element.closest(USER_CONTENT)" in leave
    assert 'contentPage() && Boolean(element.closest("main")) && !element.closest(CHROME)' in leave


PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24
HEAD = ('<!doctype html><html><head><title>ForkMesh</title>\n'
        '    <link rel="icon" href="/favicon/favicon.ico" sizes="any" />\n'
        '    <link rel="icon" href="/favicon/favicon.svg" type="image/svg+xml" />\n'
        '    <link rel="apple-touch-icon" sizes="180x180" href="/favicon/apple-touch-icon.png" />\n'
        '    <link rel="manifest" href="/favicon/site.webmanifest" />\n'
        '  </head><body><img src="/assets/logo.svg" alt="" /></body></html>')


def branded_dist(tmp_path):
    (tmp_path / "assets").mkdir(parents=True)
    (tmp_path / "favicon").mkdir()
    (tmp_path / "assets" / "logo.svg").write_text("<svg>forkmesh</svg>")
    (tmp_path / "favicon" / "site.webmanifest").write_text(json.dumps(
        {"name": "ForkMesh", "short_name": "ForkMesh", "theme_color": "#090909",
         "icons": [{"src": "/favicon/android-chrome-192x192.png"}]}))
    (tmp_path / "index.html").write_text(HEAD)
    return tmp_path


def test_brand_tool_puts_a_raster_logo_everywhere_the_mark_was(tmp_path):
    dist = branded_dist(tmp_path / "dist")
    logo = tmp_path / "logo.png"
    logo.write_bytes(PNG)
    assert apply_instance_brand.apply(dist, "Acme Code", logo) == 1
    assert (dist / "instance-logo.png").read_bytes() == PNG
    encoded = base64.b64encode(PNG).decode()
    for mark in ("assets/logo.svg", "favicon/favicon.svg"):
        assert 'href="data:image/png;base64,%s"' % encoded in (dist / mark).read_text()
    page = (dist / "index.html").read_text()
    assert "/favicon/favicon" not in page and "apple-touch-icon.png" not in page
    assert page.count('href="/instance-logo.png"') == 2
    assert 'type="image/png"' in page and 'rel="manifest"' in page
    assert '<img src="/assets/logo.svg"' in page
    manifest = json.loads((dist / "favicon" / "site.webmanifest").read_text())
    assert manifest["name"] == manifest["short_name"] == "Acme Code"
    assert manifest["icons"] == [{"src": "/instance-logo.png", "sizes": "any", "type": "image/png"}]
    assert manifest["theme_color"] == "#090909"
    assert apply_instance_brand.apply(dist, "Acme Code", logo) == 0


def test_brand_tool_uses_an_svg_logo_as_it_is_and_refuses_unsafe_ones(tmp_path, monkeypatch):
    monkeypatch.setenv("PATH", str(tmp_path / "no-tools"))
    dist = branded_dist(tmp_path / "dist")
    (dist / "instance-logo.png").write_bytes(PNG)
    svg = b'<svg xmlns="http://www.w3.org/2000/svg"><circle r="4"/></svg>'
    logo = tmp_path / "logo.svg"
    logo.write_bytes(svg)
    apply_instance_brand.apply(dist, "Acme Code", logo)
    assert (dist / "assets" / "logo.svg").read_bytes() == svg
    assert (dist / "instance-logo.svg").read_bytes() == svg
    assert not (dist / "instance-logo.png").exists()
    assert 'type="image/svg+xml"' in (dist / "index.html").read_text()
    for bad in (b"<svg><script>alert(1)</script></svg>", b"GIF89a" + b"\x00" * 20,
                PNG + b"\x00" * apply_instance_brand.MAX_LOGO_BYTES):
        logo.write_bytes(bad)
        with pytest.raises(SystemExit):
            apply_instance_brand.apply(dist, "Acme Code", logo)


def test_brand_tool_gives_an_svg_logo_a_png_copy_for_the_phone_apps(tmp_path, monkeypatch):
    tools = tmp_path / "bin"
    tools.mkdir()
    fake = tools / "rsvg-convert"
    fake.write_text("#!/bin/sh\nprintf '%%s ' \"$@\" > %s\nprintf '\\211PNG\\r\\n\\032\\nraster'\n" % (tools / "args"))
    fake.chmod(0o755)
    monkeypatch.setenv("PATH", str(tools))
    dist = branded_dist(tmp_path / "dist")
    logo = tmp_path / "logo.svg"
    logo.write_bytes(b'<svg xmlns="http://www.w3.org/2000/svg"><circle r="4"/></svg>')
    apply_instance_brand.apply(dist, "Acme Code", logo)
    assert (dist / "instance-logo.png").read_bytes() == b"\x89PNG\r\n\x1a\nraster"
    assert (tools / "args").read_text().split() == ["--width", "512", "--height", "512", "--keep-aspect-ratio"]
    manifest = json.loads((dist / "favicon" / "site.webmanifest").read_text())
    assert manifest["icons"] == [
        {"src": "/instance-logo.png", "sizes": "512x512", "type": "image/png"},
        {"src": "/instance-logo.svg", "sizes": "any", "type": "image/svg+xml"}]
    assert 'href="/instance-logo.svg"' in (dist / "index.html").read_text()
    fake.write_text("#!/bin/sh\nexit 1\n")
    apply_instance_brand.apply(dist, "Acme Code", logo)
    assert not (dist / "instance-logo.png").exists()


def test_branded_email_shows_the_instance_logo():
    helper = ENTRY.split("def instance_logo_path(env):", 1)[1].split("\n\n\n", 1)[0]
    assert 'r"/instance-logo\\.(?:png|jpg|webp|svg)", path)' in helper
    brand = ENTRY.split("def _instance_brand_email(env, text, html):", 1)[1].split("\n\n\n", 1)[0]
    assert 'logo = instance_logo_path(env) or "assets/logo-96.png"' in brand
    assert "site + logo)" in brand


def test_instance_api_names_the_logo_apps_use_as_the_project_icon():
    import re
    names = {"re": re}
    for helper in ("instance_name", "instance_branded", "instance_logo_path", "instance_logo_url"):
        source = "def " + helper + "(env):" + ENTRY.split("def " + helper + "(env):", 1)[1].split("\n\n\n", 1)[0]
        exec(source, names)
    logo_url = names["instance_logo_url"]
    assert logo_url(env()) == "/assets/logo-96.png"
    assert logo_url(env(INSTANCE_NAME="Acme Code")) == ""
    assert logo_url(env(INSTANCE_NAME="Acme Code", INSTANCE_LOGO="/instance-logo.png")) == "/instance-logo.png"
    assert logo_url(env(INSTANCE_NAME="Acme Code", INSTANCE_LOGO="/etc/passwd")) == ""
    route = APP_RUNTIME.split('if url.path in ("/api/instance", "/api/instance/"):', 1)[1].split("cache_control=", 1)[0]
    assert '"logo": instance_logo_url(self.env)' in route


def test_blog_genie_and_status_are_plugins_that_start_off_on_an_instance():
    assert {"blog", "genie", "status"} <= set(instance_features.IDS)
    branded = env(INSTANCE_NAME="Acme Code")
    for path, plugin in (
            ("/rss.xml", "blog"), ("/feed.xml", "blog"),
            ("/blog/rss.xml", "blog"), ("/api/blog/posts", "blog"),
            ("/status", "status"), ("/api/status", "status"),
            ("/api/status/world", "status"), ("/api/status/ws", "status")):
        assert instance_features.blocked(branded, path) == plugin
    assert instance_features.blocked(branded, "/blog/a-post") == "website"
    assert instance_features.blocked(branded, "/statuses") == ""
    described = {item["id"]: item for item in instance_features.describe(branded)}
    assert described["genie"]["enabled"] is False
    assert described["blog"]["title"] == "Latest from the blog"
    chosen = env(INSTANCE_NAME="Acme Code", INSTANCE_PLUGINS="status,blog,genie")
    assert instance_features.blocked(chosen, "/status") == ""
    assert instance_features.blocked(chosen, "/rss.xml") == ""
    assert {item["id"] for item in instance_features.describe(chosen)
            if item["enabled"]} == {"status", "blog", "genie"} | \
        instance_features.DEFAULT_ON
    assert instance_features.blocked(env(), "/status") == ""
    assert instance_features.blocked(env(), "/rss.xml") == ""


def test_developer_docs_is_a_plugin_that_starts_off_on_an_instance():
    branded = env(INSTANCE_NAME="Acme Code")
    for path in ("/dashboard/docs", "/dashboard/docs/", "/api/docs-book",
                 "/api/docs-book/save"):
        assert instance_features.blocked(branded, path) == "developer-docs"
    assert instance_features.blocked(branded, "/docs") == ""
    assert instance_features.blocked(branded, "/dashboard/docsx") == ""
    described = {item["id"]: item for item in instance_features.describe(branded)}
    assert described["developer-docs"]["enabled"] is False
    assert "/dashboard/docs" in described["developer-docs"]["paths"]
    chosen = env(INSTANCE_NAME="Acme Code", INSTANCE_PLUGINS="developer-docs")
    assert instance_features.blocked(chosen, "/dashboard/docs") == ""
    assert instance_features.blocked(env(), "/api/docs-book") == ""
    sidebar = (ROOT / "public" / "dashboard" / "partials" / "sidebar.html").read_text(
        encoding="utf-8")
    assert 'href="/dashboard/docs"' in sidebar


def test_plugin_gate_runs_before_the_status_snapshot_fast_path():
    fetch = APP_RUNTIME.split("    async def fetch(self, request):", 1)[1]
    head = fetch.split("_stateless_status_snapshot_response(", 1)[0]
    assert "instance_features.branded(self.env)" in head
    assert "instance_features.blocked(self.env, plugin_path)" in head
    assert "self._instance_feature_off(plugin_path, switched_off)" in head


def test_dashboard_marks_plugin_ui_and_the_brand_script_hides_it():
    views = ROOT / "public" / "dashboard" / "partials" / "views"
    home = (views / "home.html").read_text(encoding="utf-8")
    settings = (views / "settings.html").read_text(encoding="utf-8")
    assert 'data-home-blog-card data-instance-plugin="blog"' in home
    assert 'data-genie-setting data-instance-plugin="genie"' in settings
    built = (ROOT / "public" / "dashboard" / "index.html").read_text(encoding="utf-8")
    assert 'data-instance-plugin="blog"' in built
    assert "hidePlugins(null);" in BRAND_JS
    assert "[data-instance-plugin] { display: none !important; }" in BRAND_JS
    assert '[data-instance-plugin~="${id}"]' in BRAND_JS
    assert ".fm-genie, .fm-genie-bubble" in BRAND_JS
    assert "/api/forkbot" not in BRAND_JS


def test_genie_spider_asks_the_instance_before_it_builds():
    spider = (ROOT.parent / "www" / "public" / "genie-spider.js").read_text(encoding="utf-8")
    assert 'querySelector?.("script[data-instance-brand]")' in spider
    assert 'plugin.id === "genie" && plugin.enabled' in spider
    assert "return !hidden() && instanceAllows();" in spider


def test_powered_by_badge_can_be_switched_off(tmp_path):
    page = tmp_path / "index.html"
    page.write_text("<!doctype html><html><head><title>ForkMesh</title></head><body></body></html>")
    assert apply_instance_brand.apply(tmp_path, "Acme Code", powered_by=False) == 1
    assert 'data-instance-powered-by="off"' in page.read_text()
    assert apply_instance_brand.apply(tmp_path, "Acme Code") == 1
    assert 'data-instance-powered-by="off"' not in page.read_text()
    assert page.read_text().count("/instance-brand.js") == 1
    assert 'script.dataset.instancePoweredBy === "off"' in BRAND_JS
    assert 'INSTANCE_POWERED_BY' in APP_RUNTIME
    installer = (ROOT.parent / "www" / "public" / "install-instance.sh").read_text()
    assert 'c.get("poweredBy", True)' in installer
    assert "--no-powered-by" in installer
    assert 'echo "INSTANCE_POWERED_BY=$CFG_poweredBy"' in installer
