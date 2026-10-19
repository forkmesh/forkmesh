import asyncio
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import instance_directory  # noqa: E402
import instance_launch  # noqa: E402
from test_instance_launch import (  # noqa: E402
    PNG, SVG, Runtime, data_url, form, post)

DONE = [
    {"out": 'FMI {"step":"system","state":"running"}'},
    {"out": 'FMI {"done":true,"url":"https://code.example.com/acme/forkmesh"}'},
    {"exit": 0},
]


def launch(runtime, events=DONE, **changes):
    runtime.install_events = list(events)
    runtime.client = "client-%d" % runtime.ids
    answer = post(runtime, form(**changes))
    assert answer["status"] == 202, answer
    runtime.run_spawned()
    return answer["data"]["id"]


def listed(runtime):
    runtime.use(b"", method="GET")
    answer = asyncio.run(instance_launch.handle(runtime, ("instances",)))
    assert answer["status"] == 200 and answer["maxAge"] == 60
    return answer["data"]["instances"]


def logo(runtime, entry_id):
    runtime.use(b"", method="GET")
    return asyncio.run(instance_launch.handle(runtime, ("logo", entry_id)))


def test_both_choices_are_on_unless_turned_off():
    result, problem = instance_launch.validate(form())
    assert problem is None and result["ownWorld"] and result["listOnHomepage"]
    for off in (False, 0, "false", "off", "no", "0"):
        result, _ = instance_launch.validate(form(ownWorld=off, listOnHomepage=off))
        assert not result["ownWorld"] and not result["listOnHomepage"], off
    for on in (True, 1, "true", "yes", "", None):
        result, _ = instance_launch.validate(form(ownWorld=on))
        assert result["ownWorld"], on


def test_a_finished_launch_gets_a_hexagon_and_a_homepage_row():
    runtime = Runtime()
    launch_id = launch(runtime, logo=data_url(PNG))
    entries = listed(runtime)
    assert entries == [{
        "id": launch_id, "name": "Acme Code", "url": "https://code.example.com/",
        "logo": "/api/start/instances/%s/logo?v=%d" % (launch_id, runtime.clock),
        "listed": True, "world": True, "slot": 0, "worldUrl": "",
        "status": "checking", "checkedAt": 0,
        "createdAt": runtime.clock,
    }]
    image = logo(runtime, launch_id)
    assert image["type"] == "image/png" and image["body"] == PNG
    assert logo(runtime, "f" * 32)["status"] == 404
    assert logo(runtime, "../x")["status"] == 404


def test_a_failed_or_unfinished_launch_is_not_published():
    runtime = Runtime()
    launch(runtime, events=[{"out": 'FMI {"step":"system","state":"running"}'},
                            {"exit": 1}])
    launch(runtime, events=[{"exit": 0}])
    assert listed(runtime) == []


def test_each_world_gets_its_own_slot_and_keeps_it():
    runtime = Runtime()
    launch(runtime)
    launch(runtime, domain="two.example.com", projectName="Two")
    launch(runtime, domain="three.example.com", listOnHomepage=False)
    slots = {entry["url"]: (entry["slot"], entry["listed"]) for entry in listed(runtime)}
    assert slots == {"https://code.example.com/": (0, True),
                     "https://two.example.com/": (1, True),
                     "https://three.example.com/": (2, False)}
    launch(runtime, domain="two.example.com", ownWorld=False)
    launch(runtime, domain="three.example.com", projectName="Three again")
    launch(runtime, domain="four.example.com")
    entries = {entry["url"]: entry for entry in listed(runtime)}
    assert entries["https://two.example.com/"]["world"] is False
    assert entries["https://two.example.com/"]["slot"] == -1
    assert entries["https://three.example.com/"]["slot"] == 2
    assert entries["https://three.example.com/"]["name"] == "Three again"
    assert entries["https://four.example.com/"]["slot"] == 1


def test_turning_both_off_removes_the_instance():
    runtime = Runtime()
    launch(runtime)
    launch(runtime, ownWorld=False, listOnHomepage="false")
    assert listed(runtime) == []


def test_the_world_holds_twelve_hexagons():
    runtime = Runtime()
    for index in range(instance_directory.WORLD_SLOTS + 1):
        launch(runtime, domain="i%d.example.com" % index, listOnHomepage=False)
    entries = listed(runtime)
    assert sorted(entry["slot"] for entry in entries) == list(
        range(instance_directory.WORLD_SLOTS))


def test_svg_logos_keep_their_type_and_only_post_launches():
    runtime = Runtime()
    launch_id = launch(runtime, logo=data_url(SVG, "image/svg+xml"))
    assert logo(runtime, launch_id)["type"] == "image/svg+xml"
    runtime.use(b"", method="POST")
    answer = asyncio.run(instance_launch.handle(runtime, ("instances",)))
    assert answer["status"] == 405


def test_the_served_logo_cannot_run_scripts():
    source = Path(instance_launch.__file__).read_text(encoding="utf-8")
    assert "\"content-security-policy\": \"default-src 'none'; \"" in source
    assert '"x-content-type-options": "nosniff"' in source


def test_migration_creates_and_drops_the_directory():
    up = (ROOT / "migrations" / "0257_instance_directory.sql").read_text()
    down = (ROOT / "migrations" / "down" / "0257_instance_directory.sql").read_text()
    db = sqlite3.connect(":memory:")
    db.executescript(up)
    columns = [row[1] for row in db.execute("PRAGMA table_info(instance_directory)")]
    assert columns == ["domain", "id", "name", "url", "logo", "world", "listed",
                       "slot", "hidden", "failures", "offline_since",
                       "checked_at", "pinged_at", "created_at", "updated_at"]
    assert instance_directory.SCHEMA[0].split("(", 1)[1].split() == \
        up.split(";")[0].split("(", 1)[1].split()
    db.executescript(down)
    assert not db.execute(
        "SELECT name FROM sqlite_master WHERE name='instance_directory'").fetchall()


DAY = 24 * 60 * 60 * 1000


class Health(Runtime):
    def __init__(self):
        super().__init__()
        self.down = set()
        self.probed = []
        self.pings = []

    async def probe(self, url):
        self.probed.append(url)
        return url.split("/")[2] not in self.down

    async def ping_admins(self, ping):
        self.pings.append(ping)
        return 1


def check(runtime, minutes=15):
    runtime.clock += minutes * 60 * 1000
    return asyncio.run(instance_directory.check_health(runtime))


def urls(runtime):
    return sorted(entry["url"] for entry in listed(runtime))


def test_the_health_check_asks_each_instance_for_readyz():
    runtime = Health()
    launch(runtime)
    launch(runtime, domain="two.example.com")
    assert check(runtime) == {"checked": 2, "pinged": 0}
    assert sorted(runtime.probed) == ["https://code.example.com/readyz",
                                      "https://two.example.com/readyz"]


def test_an_offline_instance_is_hidden_after_two_missed_checks_and_returns():
    runtime = Health()
    launch_id = launch(runtime, logo=data_url(PNG))
    launch(runtime, domain="two.example.com")
    runtime.down.add("code.example.com")
    check(runtime)
    assert urls(runtime) == ["https://code.example.com/", "https://two.example.com/"]
    check(runtime)
    assert urls(runtime) == ["https://two.example.com/"]
    assert logo(runtime, launch_id)["status"] == 404
    runtime.down.clear()
    check(runtime)
    assert urls(runtime) == ["https://code.example.com/", "https://two.example.com/"]
    assert runtime.pings == []


def test_the_list_says_whether_each_instance_answered_its_last_check():
    runtime = Health()
    launch(runtime)
    assert [entry["status"] for entry in listed(runtime)] == ["checking"]
    check(runtime)
    assert [entry["status"] for entry in listed(runtime)] == ["online"]
    runtime.down.add("code.example.com")
    check(runtime)
    entry = listed(runtime)[0]
    assert entry["status"] == "checking"
    assert entry["checkedAt"] == runtime.clock
    runtime.down.clear()
    check(runtime)
    assert listed(runtime)[0]["status"] == "online"


def test_one_ping_asks_whether_to_delete_after_a_day_offline():
    runtime = Health()
    launch_id = launch(runtime)
    runtime.down.add("code.example.com")
    check(runtime)
    check(runtime, minutes=DAY // 60000 - 10)
    assert runtime.pings == []
    check(runtime, minutes=20)
    assert len(runtime.pings) == 1
    ping = runtime.pings[0]
    assert ping["title"] == "Instance offline: Acme Code"
    assert "Should it be deleted" in ping["body"] and "code.example.com" in ping["body"]
    assert ping["dedupe"].startswith("instance-offline:%s:" % launch_id)
    check(runtime)
    check(runtime, minutes=DAY // 60000)
    assert len(runtime.pings) == 1
    runtime.down.clear()
    check(runtime)
    runtime.down.add("code.example.com")
    check(runtime)
    check(runtime, minutes=DAY // 60000 + 20)
    assert len(runtime.pings) == 2


def test_an_admin_can_hide_show_and_delete_an_entry():
    runtime = Health()
    launch_id = launch(runtime)
    assert asyncio.run(instance_directory.set_hidden(runtime, launch_id, True)) == "code.example.com"
    assert listed(runtime) == []
    runtime.down.add("code.example.com")
    check(runtime)
    check(runtime, minutes=DAY // 60000 + 20)
    assert runtime.pings == []
    rows = asyncio.run(instance_directory.admin_rows(runtime))
    assert rows[0]["hidden"] == 1 and rows[0]["failures"] == 2
    runtime.down.clear()
    asyncio.run(instance_directory.set_hidden(runtime, launch_id, False))
    check(runtime)
    assert urls(runtime) == ["https://code.example.com/"]
    assert asyncio.run(instance_directory.delete(runtime, launch_id)) == "code.example.com"
    assert listed(runtime) == []
    assert asyncio.run(instance_directory.delete(runtime, launch_id)) is None
    assert asyncio.run(instance_directory.set_hidden(runtime, "../x", True)) is None


def test_launching_again_brings_an_offline_instance_back_but_keeps_it_hidden():
    runtime = Health()
    launch_id = launch(runtime)
    runtime.down.add("code.example.com")
    check(runtime)
    check(runtime)
    assert listed(runtime) == []
    launch(runtime)
    assert urls(runtime) == ["https://code.example.com/"]
    asyncio.run(instance_directory.set_hidden(runtime, launch_id, True))
    launch(runtime)
    assert listed(runtime) == []


def test_bad_domains_are_never_fetched():
    assert instance_directory.health_url("code.example.com") == "https://code.example.com/readyz"
    for bad in ("", "localhost", "a/b.example.com", "x.example.com:8080", "user@x.com"):
        assert instance_directory.health_url(bad) == ""


def test_the_admin_page_checks_csrf_confirms_deletes_and_audits():
    import instance_directory_admin
    source = Path(instance_directory_admin.__file__).read_text(encoding="utf-8")
    assert source.index("_admin_csrf_ok(env, form)") < source.index("instance_directory.delete(")
    assert '_first(form, "confirm") != "yes"' in source
    assert "_audit_sensitive_action(" in source
    assert '"operational_alert"' in source
    assert instance_directory_admin.VIEW == "instances"
    shell = (ROOT / "src" / "admin_shell.py").read_text(encoding="utf-8")
    assert '("instances", "Instances", "external", ("console", "instances"))' in shell
    runtime_source = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert "_instance_directory_admin.admin_handler(" in runtime_source
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    assert 'phases.append("instance-health")' in entry
    assert "return await _instance_directory_admin.health_cron(env)" in entry


def test_the_admin_page_lists_status_and_escapes_names():
    import html
    import admin_shell
    import instance_directory_admin as admin
    admin._admin_shell = admin_shell
    admin._html_escape = html.escape
    admin._admin_href = lambda _query, **params: "/admin?view=" + params["view"]
    now = 1_800_000_000_000
    base = dict(world=0, listed=1, slot=-1, hidden=0, failures=0, offline_since=0,
                checked_at=now - 60000, pinged_at=0, created_at=now - DAY)
    rows = [
        dict(base, domain="code.example.com", id="a" * 32, name="Acme Code",
             url="https://code.example.com/", world=1, slot=0),
        dict(base, domain="gone.example.org", id="b" * 32, name="Gone <b>",
             url="https://gone.example.org/", listed=0, world=1, failures=4,
             offline_since=now - 2 * DAY),
        dict(base, domain="hid.example.net", id="c" * 32, name="Hidden",
             url="https://hid.example.net/", hidden=1, checked_at=0),
    ]
    page = admin.render(rows, "<csrf>", "", now)
    assert "Homepage, World hexagon 1" in page and "Online" in page
    assert "Offline, not shown" in page and "World (no free hexagon)" in page
    assert "Hidden by an admin" in page and ">Show</button>" in page
    assert "Gone &lt;b&gt;" in page and "Gone <b>" not in page
    assert page.count('name="action" value="delete_instance"') == 3
    assert page.count('name="confirm" value="yes" required') == 3
    assert page.count("<csrf>") == 6
