import asyncio
import base64
import html
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlencode

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "host"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import admin_shell  # noqa: E402
import app_runtime  # noqa: E402
import instance_console as console_module  # noqa: E402
import instance_directory  # noqa: E402
import instance_features  # noqa: E402
import instance_launch  # noqa: E402
import instance_registry as registry  # noqa: E402
from test_instance_launch import NOW, PNG, Runtime  # noqa: E402

RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
DOMAIN = "code.example.org"
SCRIPT_SVG = b'<svg xmlns="http://www.w3.org/2000/svg"><script>x</script></svg>'


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def unb64(text):
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


class Key:
    def __init__(self):
        self.private = Ed25519PrivateKey.generate()
        self.public = b64(self.private.public_key().public_bytes_raw())

    def sign(self, message):
        return b64(self.private.sign(message))


async def verify(public_key, signature, message):
    try:
        Ed25519PublicKey.from_public_bytes(unb64(public_key)).verify(
            unb64(signature), message)
    except (InvalidSignature, ValueError):
        return False
    return True


class Directory(Runtime):
    def __init__(self):
        super().__init__()
        self.pages = {}
        self.fetched = []
        self.addresses[DOMAIN] = ["93.184.216.34"]
        self.addresses["lan.example.org"] = ["10.0.0.7"]
        self.addresses["two.example.org"] = ["93.184.216.35"]
        self.directory = True

    def serves_directory(self):
        return self.directory

    def world_home(self):
        return "https://world.example.com/world/"

    async def verify(self, public_key, signature, message):
        return await verify(public_key, signature, message)

    async def fetch_bytes(self, url, limit):
        self.fetched.append(url)
        page = self.pages.get(url)
        if page is None:
            return None
        return page


def doc(domain=DOMAIN, key=None, **changes):
    value = {
        "forkmeshInstance": 1, "domain": domain,
        "url": "https://%s/" % domain, "name": "Acme Code",
        "logo": "/instance-logo.png", "listed": True, "world": True,
        "worldUrl": "https://%s/world/" % domain,
        "publicKey": key.public if key else "A" * 43,
    }
    value.update(changes)
    return value


def serve(directory, value, domain=DOMAIN, status=200):
    directory.pages["https://%s%s" % (domain, registry.DOC_PATH)] = (
        status, json.dumps(value).encode())


def announcement(key, domain=DOMAIN, ts=NOW, nonce="ab" * 16):
    return registry.announcement_body(
        domain, ts, nonce, key.sign(registry.message(domain, ts, nonce)))


async def ring_async(directory, body, client="client-a"):
    directory.client = client
    directory.use(json.dumps(body).encode(), method="POST")
    answer = await instance_launch.handle(directory, ("register",))
    return answer["status"], answer["data"]


def ring(directory, body, client="client-a"):
    return asyncio.run(ring_async(directory, body, client))


def listed(directory):
    directory.use(b"", method="GET")
    return asyncio.run(instance_launch.handle(
        directory, ("instances",)))["data"]["instances"]


def registrations(directory):
    return [dict(row) for row in directory.db.execute(
        "SELECT * FROM instance_registrations")]


def joined(directory, key, **changes):
    serve(directory, doc(key=key, **changes))
    directory.pages["https://%s/instance-logo.png" % DOMAIN] = (200, PNG)
    return ring(directory, announcement(key))


def test_the_route_is_registered_next_to_the_instance_list():
    assert instance_launch.match("instances/register") == ("register",)
    assert instance_launch.match("instances/register/x") is None


def test_a_signed_announcement_lists_the_instance_and_gives_it_a_hexagon():
    directory, key = Directory(), Key()
    status, reply = joined(directory, key)
    assert status == 200, reply
    assert reply["ok"] is True and reply["status"] == "listed"
    assert reply["slot"] == 0 and reply["world"] and reply["listed"]
    assert reply["worldHome"] == "https://world.example.com/world/"
    assert reply["hidden"] is False
    entries = listed(directory)
    assert [(item["name"], item["url"], item["slot"]) for item in entries] == [
        ("Acme Code", "https://%s/" % DOMAIN, 0)]
    assert entries[0]["id"] == reply["id"]
    assert entries[0]["worldUrl"] == "https://%s/world/" % DOMAIN
    assert entries[0]["logo"].startswith("/api/start/instances/")
    stored = registrations(directory)
    assert len(stored) == 1 and stored[0]["public_key"] == key.public
    assert stored[0]["world_url"] == "https://%s/world/" % DOMAIN
    assert stored[0]["announced_at"] == NOW
    assert directory.fetched == [
        "https://%s/.well-known/forkmesh-instance.json" % DOMAIN,
        "https://%s/instance-logo.png" % DOMAIN]


def test_announcing_again_updates_the_name_and_keeps_the_id_and_hexagon():
    directory, key = Directory(), Key()
    first = joined(directory, key)[1]
    directory.clock += 60_000
    serve(directory, doc(key=key, name="Acme Git"))
    status, again = ring(directory, announcement(key, ts=directory.clock))
    assert status == 200 and again["id"] == first["id"]
    assert again["slot"] == first["slot"]
    assert listed(directory)[0]["name"] == "Acme Git"


def test_a_world_address_is_published_only_with_a_hexagon():
    directory, key = Directory(), Key()
    joined(directory, key, worldUrl="")
    assert listed(directory)[0]["worldUrl"] == ""
    directory.clock += 60_000
    serve(directory, doc(key=key, world=False, worldUrl=""))
    ring(directory, announcement(key, ts=directory.clock))
    assert listed(directory)[0]["world"] is False
    assert listed(directory)[0]["worldUrl"] == ""
    row = {"url": "https://a.example.org/",
           "world_url": "https://b.example.org/world/"}
    assert instance_directory.world_url(row, True) == ""
    row["world_url"] = "https://a.example.org/world/"
    assert instance_directory.world_url(row, True) == row["world_url"]
    assert instance_directory.world_url(row, False) == ""


def test_turning_both_off_removes_the_instance():
    directory, key = Directory(), Key()
    joined(directory, key)
    directory.clock += 60_000
    serve(directory, doc(key=key, listed=False, world=False, worldUrl=""))
    status, reply = ring(directory, announcement(key, ts=directory.clock))
    assert (status, reply) == (200, {"ok": True, "status": "removed"})
    assert listed(directory) == [] and registrations(directory) == []


def test_a_signature_by_another_key_changes_nothing():
    directory, key, other = Directory(), Key(), Key()
    serve(directory, doc(key=key))
    status, reply = ring(directory, announcement(other))
    assert status == 401 and reply["error"] == "bad_signature"
    assert listed(directory) == [] and registrations(directory) == []


def test_a_signature_for_another_domain_or_time_does_not_count():
    directory, key = Directory(), Key()
    serve(directory, doc(key=key))
    body = announcement(key)
    body["ts"] = NOW + 1
    assert ring(directory, body)[0] == 401
    body = announcement(key, domain="two.example.org")
    body["domain"] = DOMAIN
    assert ring(directory, body)[0] == 401


def test_only_the_domain_itself_decides_through_its_document():
    directory, key = Directory(), Key()
    joined(directory, key)
    directory.clock += 60_000
    del directory.pages["https://%s%s" % (DOMAIN, registry.DOC_PATH)]
    status, reply = ring(directory, announcement(key, ts=directory.clock))
    assert status == 502 and reply["error"] == "unreachable"
    serve(directory, {"error": "not_found"}, status=404)
    directory.clock += 60_000
    assert ring(directory, announcement(key, ts=directory.clock))[0] == 502
    assert len(listed(directory)) == 1


@pytest.mark.parametrize("change", [
    {"domain": "evil.example.org"},
    {"url": "https://evil.example.org/"},
    {"forkmeshInstance": 2},
    {"listed": "yes"},
    {"name": ""},
    {"name": "x" * 81},
    {"logo": "https://evil.example.org/logo.png"},
    {"logo": "/../../etc/passwd"},
    {"worldUrl": "https://evil.example.org/world/"},
    {"world": False, "worldUrl": "https://code.example.org/world/"},
    {"publicKey": "short"},
])
def test_a_document_with_a_bad_field_is_refused(change):
    directory, key = Directory(), Key()
    value = doc(key=key)
    value.update(change)
    serve(directory, value)
    status, reply = ring(directory, announcement(key))
    assert status == 400 and reply["error"] == "invalid_document", reply
    assert listed(directory) == []


def test_a_document_that_is_not_json_or_too_large_is_refused():
    directory, key = Directory(), Key()
    directory.pages["https://%s%s" % (DOMAIN, registry.DOC_PATH)] = (
        200, b"<!doctype html><title>app</title>")
    assert ring(directory, announcement(key))[1]["error"] == "invalid_document"
    directory.pages["https://%s%s" % (DOMAIN, registry.DOC_PATH)] = (
        200, b" " * (registry.DOC_MAX_BYTES + 1))
    directory.clock += 1
    status, reply = ring(directory, announcement(key, ts=directory.clock))
    assert status == 400 and reply["error"] == "too_large"


def test_old_or_replayed_announcements_are_refused():
    directory, key = Directory(), Key()
    serve(directory, doc(key=key))
    stale = announcement(key, ts=NOW - registry.MAX_SKEW_MS - 1)
    assert ring(directory, stale)[1]["error"] == "stale"
    future = announcement(key, ts=NOW + registry.MAX_SKEW_MS + 1)
    assert ring(directory, future)[1]["error"] == "stale"
    body = announcement(key)
    assert ring(directory, body)[0] == 200
    status, reply = ring(directory, body)
    assert status == 409 and reply["error"] == "replayed"


@pytest.mark.parametrize("body", [
    {}, [], {"domain": "localhost", "ts": NOW, "nonce": "ab" * 16,
             "signature": "s" * 86},
    {"domain": DOMAIN, "ts": "now", "nonce": "ab" * 16, "signature": "s" * 86},
    {"domain": DOMAIN, "ts": True, "nonce": "ab" * 16, "signature": "s" * 86},
    {"domain": DOMAIN, "ts": NOW, "nonce": "zz", "signature": "s" * 86},
    {"domain": DOMAIN, "ts": NOW, "nonce": "ab" * 16, "signature": "short"},
    {"domain": "app.forkmesh.com", "ts": NOW, "nonce": "ab" * 16,
     "signature": "s" * 86},
    {"domain": "forkmesh.com", "ts": NOW, "nonce": "ab" * 16,
     "signature": "s" * 86},
])
def test_malformed_announcements_are_refused_before_any_fetch(body):
    directory = Directory()
    status, reply = ring(directory, body)
    assert status == 400 and reply["ok"] is False, reply
    assert directory.fetched == []


def test_a_domain_on_a_private_address_is_never_fetched():
    directory, key = Directory(), Key()
    status, reply = ring(directory, announcement(key, domain="lan.example.org"))
    assert status == 400 and reply["error"] == "address_not_public"
    assert directory.fetched == []
    status, reply = ring(directory, announcement(key, domain="nx.example.org"))
    assert reply["error"] == "address_not_public"


def test_announcements_are_rate_limited_per_domain_and_per_client():
    directory, key = Directory(), Key()
    serve(directory, doc(key=key), status=503)
    for index in range(registry.DOMAIN_PER_HOUR):
        directory.clock += 1
        ring(directory, announcement(key, ts=directory.clock),
             client="c%d" % index)
    directory.clock += 1
    status, reply = ring(directory, announcement(key, ts=directory.clock),
                         client="fresh")
    assert status == 429 and reply["error"] == "rate_limited"
    fetches = len(directory.fetched)
    directory.clock += registry.HOUR_MS
    serve(directory, doc(key=key))
    assert ring(directory, announcement(key, ts=directory.clock))[0] == 200
    other = Directory()
    for index in range(registry.CLIENT_PER_HOUR):
        ring(other, {"domain": "d%d.example.org" % index, "ts": NOW,
                     "nonce": "ab" * 16, "signature": "s" * 86}, "same")
    status, _ = ring(other, announcement(key), "same")
    assert status == 429 and fetches == registry.DOMAIN_PER_HOUR


def test_a_bad_logo_is_left_out_but_the_instance_is_listed():
    directory, key = Directory(), Key()
    serve(directory, doc(key=key, logo="/instance-logo.svg"))
    directory.pages["https://%s/instance-logo.svg" % DOMAIN] = (
        200, SCRIPT_SVG)
    status, reply = ring(directory, announcement(key))
    assert status == 200 and listed(directory)[0]["logo"] == ""


def test_an_admin_hide_survives_announcements_and_a_delete_lets_it_return():
    directory, key = Directory(), Key()
    entry_id = joined(directory, key)[1]["id"]
    asyncio.run(instance_directory.set_hidden(directory, entry_id, True))
    directory.clock += 60_000
    status, reply = ring(directory, announcement(key, ts=directory.clock))
    assert status == 200 and reply["hidden"] is True
    assert listed(directory) == []
    asyncio.run(instance_directory.delete(directory, entry_id))
    assert registrations(directory) == []
    directory.clock += 60_000
    assert ring(directory, announcement(key, ts=directory.clock))[0] == 200
    assert len(listed(directory)) == 1


def test_self_registered_instances_are_health_checked_like_launched_ones():
    directory, key = Directory(), Key()
    joined(directory, key)
    probed = []

    async def probe(url):
        probed.append(url)
        return True
    directory.probe = probe
    directory.clock += 15 * 60 * 1000
    asyncio.run(instance_directory.check_health(directory))
    assert probed == ["https://%s/readyz" % DOMAIN]


def test_an_instance_does_not_serve_the_register_route():
    directory, key = Directory(), Key()
    directory.directory = False
    serve(directory, doc(key=key))
    assert ring(directory, announcement(key))[0] == 404
    assert directory.fetched == []


def test_the_registration_tables_match_their_migration():
    up = (ROOT / "migrations" / "0273_instance_registrations.sql").read_text()
    statements = [item.strip() for item in up.split(";") if item.strip()]
    schema = [" ".join(item.split()) for item in instance_directory.SCHEMA[3:]]
    assert schema == [" ".join(item.split()) for item in statements]
    down = (ROOT / "migrations" / "down" /
            "0273_instance_registrations.sql").read_text()
    assert "DROP TABLE IF EXISTS instance_registrations" in down
    assert "DROP TABLE IF EXISTS instance_register_attempts" in down


def setting(**changes):
    value = {"listed": True, "world": True, "status": {}}
    value.update(changes)
    return registry.parse_setting(registry.dump_setting(value))


def test_the_instance_setting_reads_strictly_and_keeps_its_status():
    assert registry.parse_setting("") == {
        "listed": False, "world": False,
        "status": registry.clean_status({})}
    assert registry.parse_setting("{")["listed"] is False
    assert registry.parse_setting('{"listed": "yes"}')["listed"] is False
    status = registry.clean_status({
        "state": "listed", "slot": 99, "id": "../x",
        "worldHome": "javascript:alert(1)", "message": "m" * 999})
    assert status["slot"] == -1 and status["id"] == ""
    assert status["worldHome"] == "" and len(status["message"]) == 300
    assert registry.from_install(None) is None
    assert registry.from_install({"listed": True})["world"] is False
    for bad in ({"listed": 1}, {"other": True}, ["listed"]):
        with pytest.raises(ValueError):
            registry.from_install(bad)


def test_an_instance_announces_when_its_choice_changed_or_every_six_hours():
    fresh = setting()
    assert registry.due(fresh, NOW)
    done = setting(status={"state": "listed", "at": NOW,
                           "sent": {"listed": True, "world": True}})
    assert not registry.due(done, NOW + registry.REANNOUNCE_MS - 1)
    assert registry.due(done, NOW + registry.REANNOUNCE_MS)
    changed = setting(world=False, status=done["status"])
    assert registry.due(changed, NOW + 1)
    failed = setting(status={"state": "failed", "at": NOW})
    assert not registry.due(failed, NOW + registry.RETRY_MS - 1)
    assert registry.due(failed, NOW + registry.RETRY_MS)
    leaving = setting(listed=False, world=False, status=done["status"])
    assert registry.due(leaving, NOW + 1)
    left = setting(listed=False, world=False,
                   status={"state": "removed", "at": NOW})
    assert not registry.due(left, NOW + registry.REANNOUNCE_MS * 10)
    never = setting(listed=False, world=False)
    assert not registry.due(never, NOW)


def test_the_document_offers_a_world_only_when_the_plugin_is_on():
    value = registry.document(DOMAIN, "  Acme   Code ", "/instance-logo.png",
                              setting(), True, "K" * 43)
    assert value == {
        "forkmeshInstance": 1, "domain": DOMAIN,
        "url": "https://%s/" % DOMAIN, "name": "Acme Code",
        "logo": "/instance-logo.png", "listed": True, "world": True,
        "worldUrl": "https://%s/world/" % DOMAIN, "publicKey": "K" * 43}
    assert registry.document(DOMAIN, "Acme", "", setting(), False,
                             "K" * 43)["worldUrl"] == ""
    assert registry.document(DOMAIN, "Acme", "/x.png", setting(world=False),
                             True, "K" * 43)["worldUrl"] == ""
    assert registry.document(DOMAIN, "Acme", "/x.png", setting(), True,
                             "K" * 43)["logo"] == ""
    assert registry.domain_of("https://Code.Example.org/") == DOMAIN
    assert registry.domain_of("http://code.example.org") == ""
    assert registry.directory_url("") == "https://app.forkmesh.com"
    assert registry.directory_url("https://dir.example.com/") == (
        "https://dir.example.com")
    assert registry.directory_url("http://dir.example.com") == ""


class Instance:
    def __init__(self, directory, key, choice=None, world_on=True):
        self.directory, self.key, self.world_on = directory, key, world_on
        self.doc = None if choice is None else registry.dump_setting(choice)
        self.clock = NOW
        self.posts = []
        self.down = False

    def now(self):
        return self.clock

    async def setting_doc(self):
        return self.doc

    async def save_setting_doc(self, doc):
        self.doc = doc

    def domain(self):
        return DOMAIN

    def directory_url(self):
        return "https://app.example.com"

    def public_key(self):
        return self.key.public

    def nonce(self):
        return "%032x" % len(self.posts)

    async def sign(self, message):
        return self.key.sign(message)

    async def post_json(self, url, body, limit):
        self.posts.append((url, body))
        if self.down:
            return 0, {}
        current = registry.parse_setting(self.doc)
        serve(self.directory, registry.document(
            DOMAIN, "Acme Code", "/instance-logo.png", current, self.world_on,
            self.key.public))
        self.directory.pages["https://%s/instance-logo.png" % DOMAIN] = (
            200, PNG)
        self.directory.clock = self.clock
        return await ring_async(self.directory, body)


def test_an_instance_joins_the_directory_and_leaves_it_end_to_end():
    directory, key = Directory(), Key()
    instance = Instance(directory, key, setting())
    result = asyncio.run(registry.announce_if_due(instance))
    assert result == {"announced": True, "state": "listed"}
    assert instance.posts[0][0] == (
        "https://app.example.com/api/start/instances/register")
    stored = registry.parse_setting(instance.doc)
    assert stored["status"]["state"] == "listed"
    assert stored["status"]["slot"] == 0
    assert stored["status"]["worldHome"] == "https://world.example.com/world/"
    assert registry.world_portal(stored, True) == {
        "home": "https://world.example.com/world/", "slot": 0}
    assert registry.world_portal(stored, False) is None
    assert len(listed(directory)) == 1
    instance.clock += 60_000
    assert asyncio.run(registry.announce_if_due(instance)) == {
        "announced": False, "reason": "not_due"}
    left = registry.parse_setting(instance.doc)
    left["listed"] = left["world"] = False
    instance.doc = registry.dump_setting(left)
    assert asyncio.run(registry.announce_if_due(instance))["state"] == (
        "removed")
    assert listed(directory) == []
    assert registry.world_portal(registry.parse_setting(instance.doc),
                                 True) is None


def test_an_instance_records_why_an_announcement_failed():
    directory, key = Directory(), Key()
    instance = Instance(directory, key, setting())
    instance.down = True
    asyncio.run(registry.announce(instance))
    status = registry.parse_setting(instance.doc)["status"]
    assert status["state"] == "failed"
    assert status["message"] == "forkmesh.com did not answer."
    instance.down = False
    instance.key = Key()
    serve(directory, doc(key=key))
    reply = registry.status_from_reply(
        401, {"ok": False, "message": "The signature does not match."},
        {"listed": True, "world": True}, NOW)
    assert reply["state"] == "failed" and "signature" in reply["message"]
    assert asyncio.run(registry.announce_if_due(
        Instance(directory, key, None))) == {
        "announced": False, "reason": "not_configured"}


class Answer:
    def __init__(self, body, status=200, headers=None, **_extra):
        self.body, self.status = body, status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, url, method="GET", form=None):
        self.url, self.method = url, method
        self.form = urlencode(form or {}, doseq=True)

    async def text(self):
        return self.form


CLOCK = [NOW]


def bind_console(monkeypatch, branded=True, rows=None, audits=None,
                 directory=None, key=None):
    rows = rows if rows is not None else {}
    CLOCK[0] = NOW
    directory = directory or Directory()
    key = key or Key()

    async def d1_first(_env, sql, *args):
        return {"doc": rows[args[0]]} if args and args[0] in rows else None

    async def d1_run(_env, sql, *args):
        assert "INSERT INTO instance_settings" in sql
        rows[args[0]] = args[1]

    async def audit(_env, account, action, kind, target, outcome, details):
        if audits is not None:
            audits.append((account, action, outcome, details))

    async def nav_badges(_env):
        return {}

    async def sign(_public, _seed, message):
        return key.sign(message)

    class Reply:
        def __init__(self, status, data):
            self.status, self.data = status, data

        async def text(self):
            return json.dumps(self.data)

    async def fetch(url, options, _timeout):
        body = json.loads(options["body"])
        current = registry.parse_setting(rows.get("directory") or "")
        serve(directory, registry.document(
            DOMAIN, "Acme Code", "", current, False, key.public))
        directory.clock = CLOCK[0]
        status, data = await ring_async(directory, body)
        return Reply(status, data)

    monkeypatch.setattr(admin_shell, "nav_badges", nav_badges)
    for name, value in {
        "_admin_shell": admin_shell, "_html_escape": html.escape,
        "_admin_href": lambda _query, **params: "?" + "&amp;".join(
            "%s=%s" % item for item in params.items()),
        "Date": SimpleNamespace(now=lambda: CLOCK[0]),
        "Response": Answer,
        "method_name": lambda request: request.method,
        "_admin_csrf_ok": lambda _env, form: form.get("csrf") == ["ok"],
        "instance_branded": lambda _env: branded,
        "d1_first": d1_first, "d1_run": d1_run,
        "_audit_sensitive_action": audit,
        "_https_mirror_router_public_key": lambda _env: key.public,
        "_https_mirror_router_seed": lambda _env: "seed",
        "ed25519_sign": sign,
        "js_fetch_with_timeout": fetch,
    }.items():
        monkeypatch.setattr(console_module, name, value, raising=False)
    return rows, directory


def console(form=None, action="save_directory"):
    env = SimpleNamespace(ADMIN_PATH="secret", INSTANCE_NAME="Acme Code",
                          INSTANCE_PLUGINS="",
                          PUBLIC_BASE_URL="https://%s" % DOMAIN,
                          INSTANCE_DIRECTORY_URL="https://app.example.com")
    if form is None:
        request = FakeRequest("https://%s/secret?view=federation" % DOMAIN)
    else:
        request = FakeRequest("https://%s/secret" % DOMAIN, "POST",
                              dict(form, action=action))
    return asyncio.run(console_module.admin_handler(
        env, request, {"view": ["federation"]}, "<csrf>", "", "ada"))


@pytest.fixture(autouse=True)
def fresh_cache():
    instance_features.forget()
    yield
    instance_features.forget()


def test_the_console_saves_the_listing_and_announces_at_once(monkeypatch):
    audits = []
    rows, directory = bind_console(monkeypatch, audits=audits)
    page = console().body
    assert 'name="listed"' in page and 'name="world"' in page
    assert "Not announced yet" in page and "announce_directory" not in page
    answer = console({"csrf": "ok", "listed": "1", "world": "1"})
    assert answer.headers["location"] == (
        "/secret?view=federation&notice=directory_listed")
    assert len(listed(directory)) == 1
    assert audits[-1] == ("ada", "admin.console_save_directory", "success",
                          {"listed": True, "world": True, "announced": True,
                           "state": "listed",
                           "notice": "directory_listed"})
    page = console().body
    assert "World hexagon 1" in page and "announce_directory" in page
    CLOCK[0] += 60_000
    answer = console({"csrf": "ok"})
    assert answer.headers["location"].endswith("notice=directory_removed")
    assert listed(directory) == []
    answer = console({"csrf": "ok"}, action="announce_directory")
    assert answer.headers["location"].endswith("notice=directory_saved")


def test_the_console_refuses_forged_posts_and_forkmesh_com(monkeypatch):
    rows, directory = bind_console(monkeypatch)
    answer = console({"csrf": "bad", "listed": "1"})
    assert answer.headers["location"].endswith("notice=csrf")
    assert rows == {} and directory.fetched == []
    rows, directory = bind_console(monkeypatch, branded=False)
    assert "save_directory" not in console().body
    answer = console({"csrf": "ok", "listed": "1"})
    assert answer.headers["location"].endswith("notice=directory_core")
    assert rows == {}
    assert asyncio.run(console_module.announce_cron(SimpleNamespace())) == {
        "announced": False, "reason": "directory_host"}


def test_the_hourly_cron_announces_only_when_due():
    catalog = (ROOT / "src" / "cron_activity.py").read_text()
    assert '("instance-announce", 60, (32,), "nodes",' in catalog
    assert '        phases.append("instance-announce")' in ENTRY
    assert "return await _instance_console.announce_cron(env)" in ENTRY


def test_an_instance_serves_its_document_and_reports_its_portal(monkeypatch):
    stored = registry.dump_setting(setting(status={
        "state": "listed", "at": NOW, "slot": 4,
        "worldHome": "https://world.example.com/world/",
        "sent": {"listed": True, "world": True}}))
    reads = {"doc": stored}

    async def d1_first(_env, sql, *args):
        return {"doc": reads["doc"]} if reads["doc"] is not None else None

    def json_response(data, status=200, cache_control=None, **_extra):
        return Answer(data, status, {"cache-control": cache_control})

    for name, value in {
        "d1_first": d1_first, "json_response": json_response,
        "instance_registry": registry,
        "instance_features": instance_features,
        "instance_name": lambda _env: "Acme Code",
        "instance_branded": lambda _env: True,
        "_https_mirror_router_public_key": lambda _env: "K" * 43,
    }.items():
        monkeypatch.setattr(app_runtime, name, value, raising=False)
    me = SimpleNamespace(env=SimpleNamespace(
        INSTANCE_NAME="Acme Code", INSTANCE_PLUGINS="world",
        PUBLIC_BASE_URL="https://%s" % DOMAIN,
        INSTANCE_LOGO="/instance-logo.png"))
    me._without_head_body = lambda request, response: response
    me._instance_directory_setting = (
        lambda: app_runtime.Default._instance_directory_setting(me))
    answer = asyncio.run(app_runtime.Default._instance_directory_document(
        me, SimpleNamespace(method="GET")))
    assert answer.status == 200 and answer.body["worldUrl"] == (
        "https://%s/world/" % DOMAIN)
    assert answer.headers["cache-control"] == "no-store"
    links = asyncio.run(app_runtime.Default._instance_directory_links(me))
    assert links == {"listed": True, "world": True, "portal": {
        "home": "https://world.example.com/world/", "slot": 4}}
    reads["doc"] = None
    answer = asyncio.run(app_runtime.Default._instance_directory_document(
        me, SimpleNamespace(method="GET")))
    assert answer.status == 404
    assert asyncio.run(app_runtime.Default._instance_directory_links(
        me)) is None
    route = RUNTIME.split("    async def _route(self, request, url):", 1)[1]
    head = route.split("delegated = self._world_ingress_redirect", 1)[0]
    assert "url.path == instance_registry.DOC_PATH" in head
    assert '"directory": await self._instance_directory_links(),' in head
    assert 'instance_registry = _LazyModule("instance_registry")' in ENTRY
