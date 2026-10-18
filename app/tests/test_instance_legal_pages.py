import asyncio
import html
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlencode, urlparse

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))

import admin_shell  # noqa: E402
import app_runtime  # noqa: E402
import instance_legal  # noqa: E402
import server_update_admin as admin  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
PUBLIC = ROOT / "public"
BRAND_JS = (PUBLIC / "instance-brand.js").read_text(encoding="utf-8")
SIGNUP_HTML = (PUBLIC / "signup.html").read_text(encoding="utf-8")
SIGNUP_JS = (PUBLIC / "signup.js").read_text(encoding="utf-8")
SITE_HEADER = (REPO / "www" / "public" / "site-header.js").read_text(
    encoding="utf-8")
SIDEBAR = (PUBLIC / "dashboard" / "partials" / "sidebar.html").read_text(
    encoding="utf-8")
PROFILE_FOOTER = (PUBLIC / "dashboard" / "partials" / "views"
                  / "profile-overview.html").read_text(encoding="utf-8")
NOW = 1_800_000_000_000
TERMS_MD = "# Acme terms\n\nBe **kind**.\n"


@pytest.fixture(autouse=True)
def fresh_cache():
    instance_legal.forget()
    yield
    instance_legal.forget()


def setting(**pages):
    value = instance_legal.default()
    for page, entry in pages.items():
        value = instance_legal.with_page(value, page, entry)
    return value


def test_every_page_starts_off_and_a_damaged_row_reads_as_off():
    off = instance_legal.default()
    assert instance_legal.parse("") == off
    assert instance_legal.parse("{") == off
    assert instance_legal.parse("[]") == off
    assert instance_legal.parse(json.dumps(
        {"terms": {"mode": "url", "url": "javascript:alert(1)"},
         "privacy": {"mode": "markdown", "markdown": ""},
         "security": "on"})) == off
    assert all(not instance_legal.enabled(off, page)
               for page in instance_legal.IDS)
    assert instance_legal.links(off, True) == {
        page: {"enabled": False, "href": "", "external": False}
        for page in instance_legal.IDS}


def test_forkmesh_com_keeps_its_own_links_whatever_is_stored():
    stored = setting(terms=instance_legal.clean(
        "terms", "url", url="https://example.com/terms"))
    for value in (instance_legal.default(), stored):
        assert instance_legal.links(value, False) == {
            "terms": {"enabled": True, "href": "/terms", "external": False},
            "privacy": {"enabled": True, "href": "/privacy",
                        "external": False},
            "security": {"enabled": True, "href": "/security-report",
                         "external": False},
        }


def test_turned_on_pages_link_to_this_site_or_to_the_given_address():
    value = setting(
        terms=instance_legal.clean("terms", "markdown", TERMS_MD),
        privacy=instance_legal.clean("privacy", "url",
                                     url="https://example.org/privacy"))
    links = instance_legal.links(value, True)
    assert links["terms"] == {"enabled": True, "href": "/terms",
                              "external": False}
    assert links["privacy"] == {"enabled": True,
                                "href": "https://example.org/privacy",
                                "external": True}
    assert links["security"]["enabled"] is False
    assert instance_legal.parse(instance_legal.dump(value)) == value


def test_turning_a_page_off_keeps_its_text_for_later():
    entry = instance_legal.clean("terms", "off", TERMS_MD,
                                 "https://example.org/t")
    assert entry == {"mode": "off", "markdown": TERMS_MD,
                     "url": "https://example.org/t"}
    assert not instance_legal.enabled(setting(terms=entry), "terms")


@pytest.mark.parametrize("args", [
    ("terms", "markdown", "   "),
    ("terms", "url", "", ""),
    ("terms", "url", "", "javascript:alert(1)"),
    ("terms", "url", "", "ftp://example.com/terms"),
    ("terms", "url", "", "/terms"),
    ("terms", "url", "", "https://exa mple.com/"),
    ("terms", "url", "", 'https://example.com/"onmouseover='),
    ("terms", "url", "", "https://"),
    ("terms", "maybe", TERMS_MD),
    ("cookies", "markdown", TERMS_MD),
    ("terms", "markdown", "x" * (instance_legal.MAX_MARKDOWN_BYTES + 1)),
    ("security", "markdown", TERMS_MD, "", "javascript:alert(1)"),
    ("security", "markdown", TERMS_MD, "", "http://example.com/report"),
    ("security", "markdown", TERMS_MD, "", "mailto:not an address"),
])
def test_bad_settings_are_refused(args):
    with pytest.raises(instance_legal.SettingError):
        instance_legal.clean(*args)


def test_the_security_contact_is_an_email_or_an_https_address():
    clean = instance_legal.clean_contact
    assert clean("security@example.com") == "mailto:security@example.com"
    assert clean("mailto:security@example.com") == (
        "mailto:security@example.com")
    assert clean("https://example.com/report") == "https://example.com/report"
    assert clean("") == ""


def test_markdown_is_rendered_with_the_repository_renderer_and_sanitized():
    source = ("Intro <script>alert(1)</script> <img src=x onerror=alert(1)>\n\n"
              "[bad](javascript:alert(1)) [good](https://example.com/x) "
              "[mail](mailto:legal@example.com)\n\n"
              "```\n<b>kept as text</b>\n```\n")
    page = instance_legal.page_html('Acme "Code"', "terms", source)
    main = page.split("<main>", 1)[1].split("</main>", 1)[0]
    assert "<script" not in main and "<img" not in main
    assert "onerror" not in main and "javascript:" not in main
    assert '<a href="https://example.com/x">good</a>' in main
    assert '<a href="mailto:legal@example.com">mail</a>' in main
    assert "&lt;b&gt;kept as text&lt;/b&gt;" in main
    assert main.startswith("<h1>Terms</h1>")
    assert "<title>Terms - Acme &quot;Code&quot;</title>" in page
    own = instance_legal.page_html("Acme", "terms", TERMS_MD)
    assert "<h1>Acme terms</h1>" in own and "<h1>Terms</h1>" not in own
    assert "<strong>kind</strong>" in own


def test_security_txt_follows_the_security_setting():
    assert instance_legal.security_txt(
        instance_legal.default(), "https://git.example.org", NOW) == ""
    page = setting(security=instance_legal.clean("security", "markdown",
                                                 "Report to us."))
    assert instance_legal.security_txt(page, "https://git.example.org/",
                                       NOW) == (
        "Contact: https://git.example.org/security\n"
        "Expires: 2027-07-14T08:00:00Z\n"
        "Policy: https://git.example.org/security\n"
        "Canonical: https://git.example.org/.well-known/security.txt\n")
    linked = setting(security=instance_legal.clean(
        "security", "url", url="https://example.org/security",
        contact="security@example.org"))
    text = instance_legal.security_txt(linked, "https://git.example.org", NOW)
    assert text.startswith("Contact: mailto:security@example.org\n")
    assert "Policy: https://example.org/security\n" in text


def test_install_settings_turn_on_only_the_pages_they_name():
    assert instance_legal.from_install(None) is None
    assert instance_legal.from_install({}) is None
    value = instance_legal.from_install({
        "terms": {"markdown": TERMS_MD},
        "privacy": "https://example.org/privacy",
        "security": False})
    assert value["terms"]["mode"] == "markdown"
    assert value["privacy"] == {"mode": "url", "markdown": "",
                                "url": "https://example.org/privacy"}
    assert value["security"]["mode"] == "off"
    for bad in ({"cookies": {"url": "https://example.org"}},
                {"terms": {"markdown": "x", "url": "https://example.org"}},
                {"terms": {}}, {"terms": {"contact": "a@example.org",
                                          "url": "https://example.org"}},
                {"terms": {"url": "javascript:alert(1)"}}, ["terms"]):
        with pytest.raises(instance_legal.SettingError):
            instance_legal.from_install(bad)


class Answer:
    def __init__(self, body, status=200, headers=None, **_extra):
        self.body, self.status = body, status
        self.headers = dict(headers or {})


def bind_runtime(monkeypatch, branded=True, row=None, reads=None):
    async def d1_first(env, sql, *args):
        if reads is not None:
            reads.append(args)
        return {"doc": row} if row is not None else None

    for name, value in {
        "Response": Answer,
        "Date": SimpleNamespace(now=lambda: NOW),
        "method_name": lambda request: request.method,
        "instance_branded": lambda env: branded,
        "instance_name": lambda env: "Acme Code" if branded else "ForkMesh",
        "instance_legal": instance_legal,
        "d1_first": d1_first,
        "_normalized_http_origin": lambda value: str(value or "").rstrip("/"),
    }.items():
        monkeypatch.setattr(app_runtime, name, value, raising=False)


def runtime_self(env=None):
    default = app_runtime.Default

    async def not_found(url):
        return Answer("not found page", 404)

    me = SimpleNamespace(env=env or SimpleNamespace())
    me._serve_not_found_page = not_found
    me._without_head_body = lambda request, response: response
    me._instance_legal_setting = (
        lambda: default._instance_legal_setting(me))
    return me


def visit(me, path, method="GET"):
    url = urlparse("https://git.example.org" + path)
    return asyncio.run(app_runtime.Default._instance_legal_route(
        me, SimpleNamespace(method=method), url))


@pytest.mark.parametrize("path", ["/terms", "/privacy", "/security-report",
                                  "/terms/"])
def test_switched_off_pages_are_not_found_on_an_instance(monkeypatch, path):
    bind_runtime(monkeypatch)
    answer = visit(runtime_self(), path)
    assert answer.status == 404 and answer.body == "not found page"


def test_switched_off_security_leaves_its_paths_to_the_normal_routes(
        monkeypatch):
    bind_runtime(monkeypatch)
    me = runtime_self()
    assert visit(me, "/security") is None
    assert visit(me, "/.well-known/security.txt") is None
    assert visit(me, "/docs") is None
    assert visit(me, "/terms", "POST") is None


def test_turned_on_pages_render_redirect_and_publish_security_txt(
        monkeypatch):
    stored = setting(
        terms=instance_legal.clean("terms", "markdown",
                                   TERMS_MD + "<script>x</script>"),
        privacy=instance_legal.clean("privacy", "url",
                                     url="https://example.org/privacy"),
        security=instance_legal.clean("security", "markdown", "Mail us."))
    reads = []
    bind_runtime(monkeypatch, row=instance_legal.dump(stored), reads=reads)
    me = runtime_self(SimpleNamespace(APP_ORIGIN="https://git.example.org"))
    terms = visit(me, "/terms")
    assert terms.status == 200
    assert terms.headers["content-type"].startswith("text/html")
    assert "<h1>Acme terms</h1>" in terms.body and "<script>" not in (
        terms.body.split("<main>", 1)[1])
    assert "Acme Code" in terms.body
    privacy = visit(me, "/privacy")
    assert privacy.status == 302
    assert privacy.headers["location"] == "https://example.org/privacy"
    assert visit(me, "/security-report").headers["location"] == "/security"
    assert visit(me, "/terms/").headers["location"] == "/terms"
    assert visit(me, "/security").status == 200
    txt = visit(me, "/.well-known/security.txt")
    assert txt.status == 200 and txt.headers["content-type"].startswith(
        "text/plain")
    assert "Policy: https://git.example.org/security\n" in txt.body
    assert len(reads) == 1


def test_forkmesh_com_never_reads_the_setting(monkeypatch):
    reads = []
    bind_runtime(monkeypatch, branded=False, reads=reads,
                 row=instance_legal.dump(setting(terms=instance_legal.clean(
                     "terms", "url", url="https://example.org/t"))))
    me = runtime_self()
    assert asyncio.run(app_runtime.Default._instance_legal_setting(me)) == (
        instance_legal.default())
    assert reads == []


def test_the_router_serves_legal_pages_only_on_a_branded_instance():
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    head = route.split("delegated = self._world_ingress_redirect", 1)[0]
    branded = head.split("        if instance_branded(self.env):\n", 1)[1]
    assert branded.lstrip().startswith(
        "legal_page = await self._instance_legal_route(request, url)")
    assert ('"legal": instance_legal.links(\n'
            "                    await self._instance_legal_setting(),\n"
            "                    instance_branded(self.env)),") in head
    assert route.index("_instance_legal_route") < route.index(
        "external_owner = external_site_route(url.path)")
    assert 'instance_legal = _LazyModule("instance_legal")' in ENTRY


class FakeRequest:
    def __init__(self, url, method="GET", form=None):
        self.url, self.method = url, method
        self.form = urlencode(form or {})

    async def text(self):
        return self.form


def bind_admin(monkeypatch, branded=True, rows=None, audits=None):
    rows = rows if rows is not None else {}

    async def d1_first(env, sql, *args):
        return {"doc": rows[args[0]]} if args and args[0] in rows else None

    async def d1_run(env, sql, *args):
        assert "INSERT INTO instance_settings" in sql
        rows[args[0]] = args[1]

    async def audit(env, account, action, kind, target, outcome, details):
        if audits is not None:
            audits.append((account, action, outcome, details))

    async def nav_badges(env):
        return {}

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    for name, value in {
        "_admin_shell": admin_shell, "_html_escape": html.escape,
        "_admin_href": lambda _query, **params: "?" + "&amp;".join(
            "%s=%s" % item for item in params.items()),
        "Date": SimpleNamespace(now=lambda: NOW),
        "Response": Answer,
        "method_name": lambda request: request.method,
        "_admin_csrf_ok": lambda env, form: form.get("csrf") == ["ok"],
        "instance_branded": lambda env: branded,
        "d1_first": d1_first, "d1_run": d1_run,
        "_audit_sensitive_action": audit,
    }.items():
        monkeypatch.setattr(admin, name, value, raising=False)
    return rows


def console(form=None):
    if form is None:
        request = FakeRequest("https://git.example.org/secret?view=legal-pages")
    else:
        request = FakeRequest("https://git.example.org/secret", "POST",
                              dict(form, action="save_legal_page"))
    return asyncio.run(admin.admin_handler(
        SimpleNamespace(ADMIN_PATH="secret"), request,
        {"view": ["legal-pages"]}, "<csrf>", "", "ada"))


def test_the_console_page_shows_one_form_per_page_all_off(monkeypatch):
    bind_admin(monkeypatch)
    page = console().body
    assert page.count('value="save_legal_page"') == 3
    for page_id in instance_legal.IDS:
        assert 'name="page" value="%s"' % page_id in page
    assert page.count('<option value="off" selected>') == 3
    assert 'name="contact"' in page and page.count('name="contact"') == 1
    assert ">Legal pages<" in page


def test_an_admin_saves_one_page_at_a_time(monkeypatch):
    audits = []
    rows = bind_admin(monkeypatch, audits=audits)
    instance_legal.remember(instance_legal.default(), NOW)
    answer = console({"csrf": "ok", "page": "terms", "mode": "markdown",
                      "markdown": TERMS_MD})
    assert answer.status == 303
    assert answer.headers["location"] == (
        "/secret?view=legal-pages&notice=legal_saved")
    assert instance_legal.cached(NOW) is None
    stored = instance_legal.parse(rows[instance_legal.SETTING_KEY])
    assert stored["terms"]["mode"] == "markdown"
    assert stored["privacy"]["mode"] == "off"
    console({"csrf": "ok", "page": "security", "mode": "url",
             "url": "https://example.org/security",
             "contact": "security@example.org"})
    stored = instance_legal.parse(rows[instance_legal.SETTING_KEY])
    assert stored["terms"]["markdown"] == TERMS_MD
    assert stored["security"]["contact"] == "mailto:security@example.org"
    assert audits[-1] == ("ada", "admin.console_save_legal_page", "success",
                          {"page": "security", "mode": "url",
                           "notice": "legal_saved"})
    page = console().body
    assert page.count('<option value="off" selected>') == 1
    assert 'href="/terms"' in page and "Acme terms" in page


def test_bad_or_forged_writes_change_nothing(monkeypatch):
    rows = bind_admin(monkeypatch)
    answer = console({"csrf": "ok", "page": "terms", "mode": "url",
                      "url": "javascript:alert(1)"})
    assert answer.headers["location"].endswith("notice=legal_invalid")
    answer = console({"csrf": "bad", "page": "terms", "mode": "markdown",
                      "markdown": TERMS_MD})
    assert answer.headers["location"].endswith("notice=csrf")
    assert rows == {}


def test_forkmesh_com_shows_no_form_and_refuses_a_write(monkeypatch):
    rows = bind_admin(monkeypatch, branded=False)
    page = console().body
    assert "save_legal_page" not in page
    answer = console({"csrf": "ok", "page": "terms", "mode": "markdown",
                      "markdown": TERMS_MD})
    assert answer.headers["location"].endswith("notice=legal_core")
    assert rows == {}


def test_only_the_admin_console_writes_the_setting():
    keys = [item[0] for _group, items in admin_shell.NAV for item in items]
    assert "legal-pages" in keys
    assert "legal-pages" in admin.VIEWS
    admin_route = RUNTIME.split(
        "if not await _check_admin_page_auth(self.env, request):", 1)[1]
    assert "in _server_update_admin.VIEWS:" in admin_route
    writers = [line for line in RUNTIME.splitlines()
               if "instance_legal.SETTING_KEY" in line]
    assert len(writers) == 1 and "INSERT" not in RUNTIME.split(
        "async def _instance_legal_setting(self):", 1)[1].split(
        "async def _instance_theme(self):", 1)[0]


def test_the_brand_script_hides_switched_off_legal_links_until_it_knows():
    assert '"/security-report": "security"' in BRAND_JS
    assert ':is(header, nav, aside, footer) a[href="${path}"]' \
        ":not([data-instance-legal])" in BRAND_JS
    assert "[data-instance-legal-consent]:not([data-instance-legal])" in BRAND_JS
    assert "[data-instance-legal-group]:not([data-instance-legal])" in BRAND_JS
    assert "legalStyle();" in BRAND_JS.split("const start = () => {", 1)[1]
    assert "legal = (data && data.legal" in BRAND_JS
    catch = BRAND_JS.split(".catch(() => {", 1)[1].split("});", 1)[0]
    assert "legal = {};" in catch
    show = BRAND_JS.split("const showLegal = (link, page) => {", 1)[1]
    show = show.split("\n  };", 1)[0]
    assert "hide(link);" in show
    assert 'link.setAttribute("href", entry.href);' in show
    assert 'link.rel = "noopener";' in show


def test_the_signup_agreement_follows_the_instance_pages():
    label = SIGNUP_HTML.split('<label class="terms-check" for="terms-agree"',
                              1)[1].split("</label>", 1)[0]
    assert label.startswith(" data-instance-legal-consent>")
    assert "data-instance-legal-consent-text" in label
    consent = BRAND_JS.split("const legalConsent = () => {", 1)[1]
    consent = consent.split("\n  };", 1)[0]
    assert "if (!terms && !privacy) {" in consent
    assert "label.hidden = true;" in consent
    assert 'legalLink("terms", "Terms")' in consent
    assert 'box.dispatchEvent(new Event("change"' in consent
    assert ('const termsAccepted = () => termsAgree.checked || '
            'Boolean(termsAgree.closest("[hidden]"));') in SIGNUP_JS
    assert "termsAgree.checked &&" not in SIGNUP_JS
    assert "Accept the Terms and Privacy policy to continue." in SIGNUP_JS


def test_every_legal_link_is_one_the_brand_script_knows():
    assert 'class="fm-nav-legal" aria-label="Legal and security" ' \
        "data-instance-legal-group" in SITE_HEADER
    for href in ('href="/security-report"', 'href="/privacy"',
                 'href="/terms"'):
        assert href in SIDEBAR
    for href in ('href="/terms"', 'href="/privacy"', 'href="/security"'):
        assert href in PROFILE_FOOTER
    for path in ("/terms", "/privacy", "/security", "/security-report"):
        assert '"%s": ' % path in BRAND_JS
