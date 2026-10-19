import ast
import asyncio
import json
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import account_names  # noqa: E402

ENTRY_PATH = ROOT / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SETTINGS = (ROOT / "public" / "dashboard" / "partials" / "views"
            / "settings.html").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()
BOOT = "  void bootDashboard();"


def setup_function():
    account_names.forget()


def test_only_a_case_change_of_the_stored_name_is_accepted():
    assert account_names.clean_case("alicesmith", "AliceSmith") == "AliceSmith"
    assert account_names.clean_case("AliceSmith", "ALICESMITH") == "ALICESMITH"
    assert account_names.clean_case("alicesmith", "AliceSmyth") == ""
    assert account_names.clean_case("alicesmith", "Alice Smith") == ""
    assert account_names.clean_case("alicesmith", "") == ""
    assert account_names.clean_case("", "AliceSmith") == ""
    assert account_names.clean_case("alice-smith", "Alice-Smith") == "Alice-Smith"


def test_the_display_name_falls_back_to_the_stored_name():
    assert account_names.display_name(
        {"name": "alicesmith", "nameCase": "AliceSmith"}) == "AliceSmith"
    assert account_names.display_name({"name": "alicesmith"}) == "alicesmith"
    assert account_names.display_name(
        {"name": "bob", "nameCase": "AliceSmith"}) == "bob"
    assert account_names.display_name({}, "AliceSmith") == "alicesmith"
    assert account_names.display_name(None, "bob") == "bob"


def test_applying_a_case_keeps_the_identity_lowercase():
    rec = {"name": "alicesmith"}
    assert account_names.apply_case(rec, "AliceSmith") == (True, "")
    assert rec == {"name": "alicesmith", "nameCase": "AliceSmith"}
    assert account_names.apply_case(rec, "AliceSmith") == (False, "")
    assert account_names.apply_case(rec, "Mallory") == (False, "bad_name_case")
    assert rec["nameCase"] == "AliceSmith"
    assert account_names.apply_case(rec, "alicesmith") == (True, "")
    assert "nameCase" not in rec and rec["name"] == "alicesmith"
    assert account_names.apply_case(rec, "") == (False, "")


def test_batches_are_lowercase_unique_and_bounded():
    assert account_names.batch_names(["AliceSmith,bob", "alicesmith", "x y",
                                      "", "Carol"]) == [
        "alicesmith", "bob", "carol"]
    many = ",".join("user%d" % n for n in range(80))
    assert len(account_names.batch_names([many])) == account_names.MAX_BATCH
    assert account_names.publicly_named({"status": "active"})
    assert not account_names.publicly_named(
        {"status": "active", "profile_private": True})
    assert not account_names.publicly_named({"status": "reserved"})


def name_case_namespace(records):
    tree = ast.parse(ENTRY, filename=str(ENTRY_PATH))
    names = {"_account_name_case", "_account_name_cases"}
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef) and node.name in names]
    assert {node.name for node in selected} == names
    lookups = []

    async def blind_index(_env, value):
        return "bi:" + value

    async def rec_by_bi(_env, name_bi):
        lookups.append(name_bi)
        return records.get(name_bi[3:])

    def json_response(data, status=200, cache_control=None):
        return {"data": data, "status": status, "cache": cache_control}

    namespace = {"account_names": account_names, "blind_index": blind_index,
                 "_account_identity_rec_by_bi": rec_by_bi,
                 "json_response": json_response, "lookups": lookups,
                 "Date": SimpleNamespace(now=lambda: 1_800_000_000_000)}
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(ENTRY_PATH), "exec"), namespace)
    return namespace


def test_the_batch_endpoint_answers_display_casing_for_public_accounts():
    ns = name_case_namespace({
        "alicesmith": {"name": "alicesmith", "status": "active",
                       "nameCase": "AliceSmith"},
        "bob": {"name": "bob", "status": "active"},
        "hidden": {"name": "hidden", "status": "active", "nameCase": "HiDDen",
                   "profile_private": True},
    })
    answer = asyncio.run(ns["_account_name_cases"](
        None, ["ALICESMITH,bob,hidden,nobody,Bad Name"]))
    assert answer["data"] == {"ok": True, "names": {
        "alicesmith": "AliceSmith", "bob": "bob"}}
    assert ns["lookups"] == ["bi:alicesmith", "bi:bob", "bi:hidden",
                             "bi:nobody"]
    asyncio.run(ns["_account_name_cases"](None, ["alicesmith"]))
    assert len(ns["lookups"]) == 4


def test_the_server_stores_and_returns_the_casing():
    signup = ENTRY.split("    salt, phash = await hash_password(password)", 1)[1]
    signup = signup.split('rec["status"] = "active"', 1)[0]
    assert "account_names.apply_case(rec, account_names.clean_case(" in signup
    assert 'data.get("nodeName", "")' in signup
    profile = ENTRY.split("async def _account_profile(env, request):", 1)[1]
    profile = profile.split("\nasync def ", 1)[0]
    assert '"nameCase",' in profile
    assert 'account_names.apply_case(\n            rec, clean_string(data.get("nameCase", ""), MAX_NODE_NAME))' in profile
    assert 'return json_response({"error": case_error}, status=400)' in profile
    payload = ENTRY.split("async def _account_public_payload(", 1)[1]
    payload = payload.split("\nasync def ", 1)[0]
    assert '"nameCase": account_names.display_name(rec, name),' in payload
    handler = ENTRY.split("async def accounts_handler(env, request):", 1)[1]
    handler = handler.split("\nasync def ", 1)[0]
    assert '"nameCase": account_names.display_name(rec, name),' in handler
    assert 'name = clean_string(match.group(1), MAX_NODE_NAME).lower()' in handler
    assert handler.index('url.path == "/api/accounts/name-case"') < \
        handler.index("match = ACCOUNTS_RE.match(url.path)")
    assert '"nameCase": name_cases.get(str(r.get("name") or ""), ""),' in ENTRY
    assert 'account_names = _LazyModule("account_names")' in ENTRY
    assert "shown = name if is_org else account_names.display_name(rec, name)" in RUNTIME


def test_identity_storage_stays_lowercase():
    mirror = ENTRY.split("async def _mirror_account_identity_tables(", 1)[1]
    mirror = mirror.split("\nasync def ", 1)[0]
    assert 'name = clean_string(rec.get("name", ""), MAX_NODE_NAME).lower()' in mirror
    signup = ENTRY.split("async def _account_signup(", 1)[1].split(
        "\nasync def ", 1)[0]
    assert 'name = clean_string(data.get("nodeName", ""), MAX_NODE_NAME).lower()' in signup
    assert '"name": name,' in signup


def test_settings_offer_the_capitalization_field():
    assert "data-profile-page-name-case" in SETTINGS
    assert "for example AliceSmith" in SETTINGS


STUBS = """
const vm = require("vm");
const calls = [];
global.fetch = async (url) => {
  calls.push(String(url));
  return { ok: true, json: async () => ({ ok: true, names: { alicesmith: "AliceSmith" } }) };
};
global.location = { origin: "https://code.example.org", hostname: "code.example.org", pathname: "/dashboard", search: "", hash: "" };
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
const spans = [
  { attrs: { "data-user-name": "alicesmith" }, textContent: "alicesmith" },
  { attrs: { "data-user-name": "alicesmith" }, textContent: "@alicesmith" },
  { attrs: { "data-user-name": "bob" }, textContent: "bob" },
].map((node) => Object.assign(node, { getAttribute: (k) => node.attrs[k] }));
global.document = {
  body: { classList: emptyClassList, dataset: {} },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll(sel) { return sel === "[data-user-name]" ? spans : []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  dispatchEvent() { return true; },
  history: { pushState() {}, replaceState() {} },
  lucide: { createIcons() {} },
  setTimeout() {}, setInterval() {}, clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const api = global.__dashboardExports;
const out = {};
"""


def run_js(body):
    exports = ("userDisplayName, userNameHtml, rememberUserNameCase, "
               "hydrateUserNames, sessionFromAccountPayload, state")
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + exports + " };")
    assert transformed != DASHBOARD_JS
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n" + STUBS
              + "(async () => {\n" + body
              + "\nconsole.log(JSON.stringify(out));\n})();")
    result = subprocess.run(["node"], input=script, check=True,
                            capture_output=True, text=True)
    assert result.stderr == ""
    return json.loads(result.stdout.strip().splitlines()[-1])


def test_the_dashboard_renders_display_casing_and_falls_back():
    out = run_js("""
out.before = api.userDisplayName("alicesmith");
out.html = api.userNameHtml("alicesmith");
out.external = api.userNameHtml("Alice Smith <a>");
api.sessionFromAccountPayload({ nodeName: "carol", nameCase: "CaroL" });
out.session = api.userDisplayName("carol");
out.sessionAt = api.userDisplayName("@Carol");
out.rejected = api.rememberUserNameCase("carol", "Mallory");
out.still = api.userDisplayName("CAROL");
await api.hydrateUserNames(document);
out.texts = spans.map((node) => node.textContent);
out.calls = calls.slice();
await api.hydrateUserNames(document);
out.callsAgain = calls.length;
out.after = api.userDisplayName("alicesmith");
""")
    assert out["before"] == "alicesmith"
    assert out["html"] == '<span data-user-name="alicesmith">alicesmith</span>'
    assert out["external"] == "Alice Smith &lt;a&gt;"
    assert out["session"] == "CaroL" and out["sessionAt"] == "@CaroL"
    assert out["rejected"] is False and out["still"] == "CaroL"
    assert out["texts"] == ["AliceSmith", "@AliceSmith", "bob"]
    assert out["calls"] == ["/api/accounts/name-case?names=alicesmith%2Cbob"]
    assert out["callsAgain"] == 1
    assert out["after"] == "AliceSmith"
