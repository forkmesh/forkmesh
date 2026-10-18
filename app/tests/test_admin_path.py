import ast
import importlib.util
import re
import sys
from collections import Counter
from pathlib import Path
from types import SimpleNamespace

import pytest

APP = Path(__file__).resolve().parents[1]
REPO = APP.parent
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import admin_path  # noqa: E402

PRIVATE = "/manage-x7k2q9"


def instance(value=PRIVATE, **extra):
    return SimpleNamespace(INSTANCE_NAME="Acme Code", ADMIN_PATH=value, **extra)


def core(value="ops-console"):
    return SimpleNamespace(ADMIN_PATH=value)


def load_shell():
    spec = importlib.util.spec_from_file_location(
        "admin_shell_for_admin_path", SRC / "admin_shell.py")
    shell = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(shell)
    shell._bind_runtime({})
    return shell


@pytest.mark.parametrize("value", [
    PRIVATE, "/admin-0123456789abcdefghij", "/Ops_Console-2", "/" + "a" * 64,
    "/abcdefgh",
])
def test_a_valid_path_is_one_private_segment(value):
    assert admin_path.problem(value) == ""


@pytest.mark.parametrize("value", [
    "", "manage-x7k2q9", "/short", "/" + "a" * 65, "/manage/x7k2q9",
    "/manage-x7k2q9/", "/manage.x7k2q9", "/console.php", "/has space here",
    "/admin", "/dashboard", "/DASHBOARD", "/forgot-password",
    "/product-analysis", "/security-report", "/transparency", "/well-known",
    "/manage%2fx7k2", "/manage-x7k2q9?x=1",
])
def test_paths_that_could_collide_or_leak_are_refused(value):
    assert admin_path.problem(value)


def test_generated_paths_are_valid_unique_and_random():
    values = [admin_path.generate() for _ in range(500)]
    assert len(set(values)) == len(values)
    for value in values:
        assert admin_path.GENERATED_RE.fullmatch(value)
        assert admin_path.problem(value) == ""
    seen = Counter("".join(value[len("/admin-"):] for value in values))
    assert set(seen) == set(admin_path.ALPHABET)
    expected = 500 * admin_path.GENERATED_LENGTH / len(admin_path.ALPHABET)
    assert all(0.6 * expected < count < 1.4 * expected for count in seen.values())


def test_generation_rejects_biased_bytes():
    chunk = bytes([255, 252, 0, 35, 36, 251] * 6)
    assert admin_path.generate(lambda _n: chunk) == "/admin-" + "a9a9" * 5


def test_only_an_instance_with_a_valid_path_moves_the_console():
    assert admin_path.private_segment(instance()) == PRIVATE[1:]
    assert admin_path.private_segment(instance(PRIVATE[1:])) == PRIVATE[1:]
    for env in (core(), core(PRIVATE), instance(""), instance("admin"),
                instance("ops"), instance("/dashboard"),
                SimpleNamespace(INSTANCE_NAME="Acme")):
        assert admin_path.private_segment(env) == ""
        assert admin_path.sub_base(env) == "/admin"
        assert admin_path.sub_path(env, "theme") == "/admin/theme"
        assert not admin_path.legacy_hidden(env, "/admin/acquisition")
        assert admin_path.subpage(env, PRIVATE + "/theme") == ""
        assert admin_path.notification_href(env, "?table=users") == \
            "/admin?table=users"
    assert admin_path.console_base(core()) == "/ops-console"
    assert admin_path.console_base(instance()) == PRIVATE


def test_the_private_path_hides_every_admin_address():
    env = instance()
    for path in ("/admin", "/admin/", "/admin/theme", "/admin/acquisition",
                 "/admin/acquisition/", "/admin/hackathon", "/admin/x/y"):
        assert admin_path.legacy_hidden(env, path), path
    for path in ("/administrator", "/admins", "/", PRIVATE, "/api/admin/x"):
        assert not admin_path.legacy_hidden(env, path), path
    assert admin_path.subpage(env, PRIVATE + "/theme") == "theme"
    assert admin_path.subpage(env, PRIVATE + "/acquisition/") == "acquisition"
    assert admin_path.subpage(env, PRIVATE + "/hackathon") == "hackathon"
    assert admin_path.subpage(env, PRIVATE + "/other") == ""
    assert admin_path.subpage(env, PRIVATE + "x/theme") == ""
    assert admin_path.is_console(env, PRIVATE)
    assert admin_path.is_console(env, PRIVATE + "/")
    assert admin_path.is_console(env, PRIVATE + "/acquisition")
    assert not admin_path.is_console(env, "/admin")
    assert admin_path.sub_path(env, "theme") == PRIVATE + "/theme"
    assert admin_path.notification_href(env, "?table=users") == \
        PRIVATE + "?table=users"


def test_the_shell_draws_every_link_and_form_under_the_private_path():
    shell = load_shell()
    env = instance()
    page = shell.render_page(
        "Overview", "<p>x</p>", active="overview",
        console=shell.console_base(env), subpages=shell.subpage_base(env),
        next_path=PRIVATE + "?view=overview")
    assert 'action="%s/theme"' % PRIVATE in page
    assert 'href="%s/acquisition"' % PRIVATE in page
    assert 'href="%s/hackathon"' % PRIVATE in page
    assert 'href="%s?view=update"' % PRIVATE in page
    assert not re.search(r'(href|action)="/admin[/?"]', page)
    legacy = shell.render_page("Overview", "", console="/ops-console")
    assert 'action="/admin/theme"' in legacy
    assert 'href="/admin/acquisition"' in legacy


def test_the_acquisition_page_uses_the_private_path():
    spec = importlib.util.spec_from_file_location(
        "attribution_for_admin_path", SRC / "attribution.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    shell = load_shell()
    module._bind_runtime({"_admin_shell": shell})
    page = module.render_dashboard_html(
        None, "2026-10-01", "2026-10-07", "UTC", False, error="check failed",
        console=PRIVATE, subpages=PRIVATE)
    assert 'action="%s/acquisition"' % PRIVATE in page
    assert 'action="%s/theme"' % PRIVATE in page
    assert "/admin/" not in page


def _render_page_calls():
    for path in sorted(SRC.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if not isinstance(node, ast.Call):
                continue
            func = node.func
            name = func.attr if isinstance(func, ast.Attribute) else getattr(
                func, "id", "")
            if name in ("render_page", "render_admin_html"):
                yield path, node


def test_every_admin_page_passes_the_private_path_to_the_shell():
    calls = list(_render_page_calls())
    assert len(calls) >= 12
    for path, node in calls:
        names = {keyword.arg for keyword in node.keywords}
        if None in names:
            assert "subpages=subpages" in path.read_text(encoding="utf-8"), path
            continue
        assert "subpages" in names, "%s:%d" % (path.name, node.lineno)


ALLOWED_LEGACY = Counter({
    ("admin_path.py", "/admin"): 1,
    ("admin_console.py", "/admin"): 1,
    ("attribution.py", "/admin"): 1,
    ("product_analysis.py", "/admin"): 2,
    ("instance_directory_admin.py", "/admin"): 1,
    ("not_found_log.py", "/admin"): 1,
    ("hackathon_runtime.py", "/admin/hackathon"): 1,
    ("instance_features.py", "/admin/hackathon"): 1,
    ("app_runtime.py", "/admin/hackathon"): 2,
    ("app_runtime.py", "/admin/hackathon/"): 1,
    ("app_runtime.py", "/admin/theme"): 1,
    ("app_runtime.py", "/admin/acquisition"): 1,
    ("app_runtime.py", "/admin/acquisition/"): 1,
})


def test_no_console_link_is_hard_coded_to_admin():
    found = Counter()
    for path in sorted(SRC.glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"))
        docstrings = {
            id(node.body[0].value) for node in ast.walk(tree)
            if isinstance(node, (ast.Module, ast.FunctionDef,
                                 ast.AsyncFunctionDef, ast.ClassDef))
            and node.body and isinstance(node.body[0], ast.Expr)
            and isinstance(node.body[0].value, ast.Constant)}
        for node in ast.walk(tree):
            if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                    and id(node) not in docstrings
                    and re.search(r'(^|["\'=])/admin([/?"\']|$)', node.value)):
                found[(path.name, node.value)] += 1
    assert found == ALLOWED_LEGACY


def test_routing_checks_the_private_path_before_the_router():
    source = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    inner = source.split("    async def _fetch_inner(self, request):", 1)[1]
    inner = inner.split("\n    async def ", 1)[0]
    hidden = inner.index("_admin_path_rules.legacy_hidden(self.env, url.path)")
    sub = inner.index("_admin_path_rules.subpage(")
    console = inner.index("response = await self._admin(request)")
    router = inner.index("response = await self._route(request, url)")
    assert hidden < sub < console < router
    assert "_serve_not_found_page(url)" in inner[hidden:sub]
    assert "_private_console_headers(response)" in inner
    assert "if not read_only and not private_console:\n            response = " \
        "await _attribution.capture_page_view(" in inner
    assert "if not read_only and not private_console:\n            await " \
        "record_site_referral(" in inner
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    headers = entry.split("def _private_console_headers(response):", 1)[1]
    headers = headers.split("\n\n\n", 1)[0]
    for value in ('"cache-control", "no-store"',
                  '"x-robots-tag", "noindex, nofollow"',
                  '"referrer-policy", "no-referrer"'):
        assert value in headers


def test_a_switched_off_hackathon_stays_off_under_the_private_path():
    source = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    sub = source.split("    async def _admin_subpage(self, request, url, name):",
                       1)[1].split("\n    async def ", 1)[0]
    assert 'instance_features.blocked(self.env, "/admin/hackathon")' in sub
    assert sub.index("blocked") < sub.index("hackathon_admin_page_handler")


def test_anonymous_instance_details_never_name_the_console():
    source = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    block = source.split('if url.path in ("/api/instance", "/api/instance/"):',
                         1)[1].split("}, cache_control=", 1)[0]
    keys = re.findall(r'^\s+"([A-Za-z]+)":', block, re.M)
    assert keys and not any("admin" in key.lower() for key in keys)
    assert "ADMIN_PATH" not in block
    robots = (APP / "public" / "robots.txt").read_text(encoding="utf-8")
    assert "admin" not in robots.lower()


def test_only_admins_get_the_console_address():
    tree = ast.parse((SRC / "entry.py").read_text(encoding="utf-8"))
    payload = next(node for node in ast.walk(tree)
                   if isinstance(node, ast.AsyncFunctionDef)
                   and node.name == "_account_public_payload")
    for node in ast.walk(payload):
        if (isinstance(node, ast.Subscript)
                and isinstance(node.slice, ast.Constant)
                and node.slice.value == "adminUrl"):
            guard = next(parent for parent in ast.walk(payload)
                         if isinstance(parent, ast.If)
                         and node in list(ast.walk(parent)))
            assert ast.unparse(guard.test) == "is_admin"
            break
    else:
        raise AssertionError("adminUrl is not set")


def test_the_account_menu_links_admins_to_the_console():
    header = (APP / "public" / "dashboard" / "partials" / "header.html").read_text(
        encoding="utf-8")
    menu = header.split('id="accountMenuDropdown"', 1)[1].split("</div>\n", 1)[0]
    assert ('<a data-account-menu-admin href="#" class="fm-menu-item hidden">'
            "Admin console</a>") in menu
    script = (APP / "public" / "dashboard" / "js" / "04-account.js").read_text(
        encoding="utf-8")
    block = script.split('const adminMenuLink = $("[data-account-menu-admin]");',
                         1)[1].split("\n    }\n", 1)[0]
    assert 'session?.isAdmin ? (session?.adminUrl || "") : ""' in block
    assert 'classList.toggle("hidden", !consoleUrl)' in block
    built = (APP / "public" / "dashboard.js").read_text(encoding="utf-8")
    assert 'const adminMenuLink = $("[data-account-menu-admin]");' in built
    for page in ("index.html", "settings/index.html", "repo.html"):
        text = (APP / "public" / "dashboard" / page).read_text(encoding="utf-8")
        assert "data-account-menu-admin" in text, page


def test_the_installer_refuses_every_built_in_address():
    text = (REPO / "www" / "public" / "install-instance.sh").read_text(
        encoding="utf-8")
    block = text.split("reserved_paths = {", 1)[1].split("}", 1)[0]
    listed = set(re.findall(r'"([a-z-]+)"', block))
    needed = {name for name in admin_path.RESERVED
              if re.fullmatch(r"[A-Za-z0-9_-]{8,64}", name)} | {"admin"}
    assert needed <= listed


def test_admin_notifications_link_to_the_console_helper():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    assert entry.count("_admin_path_rules.notification_href(") == 3
    assert 'href="/admin' not in entry
    ids = (APP / "host" / "forkmesh_host" / "ids_alerts.py").read_text(
        encoding="utf-8")
    assert "href=_console_href(entry, env)" in ids
    sys.path.insert(0, str(APP / "host"))
    from forkmesh_host import ids_alerts
    rules = SimpleNamespace(notification_href=admin_path.notification_href)
    assert ids_alerts._console_href(
        SimpleNamespace(_admin_path_rules=rules), instance()) == PRIVATE
    assert ids_alerts._console_href(SimpleNamespace(), instance()) == "/admin"
