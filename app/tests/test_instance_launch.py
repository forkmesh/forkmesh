import asyncio
import base64
import hashlib
import json
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import instance_launch  # noqa: E402
import instance_theme  # noqa: E402
from test_runner_leases import NOW  # noqa: E402

ED_BLOB = base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519" + b"k" * 36).decode()
RSA_BLOB = base64.b64encode(b"\x00\x00\x00\x07ssh-rsa" + b"r" * 80).decode()
SSH_PASSWORD = "vps-root-password-1"  # forkmesh-secret-scan:ignore-line
ADMIN_PASSWORD = "admin-password-22"  # forkmesh-secret-scan:ignore-line
MAIL_KEY = "SG.abcdefghijklmnop.qrstuvwxyz0123456789"  # forkmesh-secret-scan:ignore-line
KEY = ("-----BEGIN OPENSSH PRIVATE KEY-----\n" + "b3BlbnNzaC1rZXktdjEAAAAA" * 4  # forkmesh-secret-scan:ignore-line
       + "\n-----END OPENSSH PRIVATE KEY-----\n")


def fingerprint(blob):
    digest = hashlib.sha256(base64.b64decode(blob)).digest()
    return "SHA256:" + base64.b64encode(digest).decode().rstrip("=")


class Runtime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (ROOT / "migrations" / "0240_instance_launches.sql").read_text()
            .split("INSERT INTO schema_meta")[0])
        self.db.executescript(
            (ROOT / "migrations" / "0257_instance_directory.sql").read_text())
        self.db.executescript(
            (ROOT / "migrations" / "0273_instance_registrations.sql")
            .read_text())
        self.clock = NOW
        self.ids = 0
        self.method_value = "POST"
        self.raw = b""
        self.actor = ""
        self.client = "client-a"
        self.audits = []
        self.requests = []
        self.spawned = []
        self.helper_available = True
        self.addresses = {"vps.example.com": ["8.8.4.4"],
                          "lan.example.com": ["192.168.1.5"],
                          "mixed.example.com": ["8.8.8.8", "10.0.0.2"]}
        self.keys = [{"type": "ssh-rsa", "key": RSA_BLOB},
                     {"type": "ssh-ed25519", "key": ED_BLOB}]
        self.install_events = []

    def use(self, raw=b"", actor="", method="POST"):
        self.raw, self.actor, self.method_value = raw, actor, method
        return self

    def method(self): return self.method_value
    def now(self): return self.clock
    def new_id(self):
        self.ids += 1
        return "%032x" % self.ids
    def response(self, data, status=200): return {"status": status, "data": data}
    async def ensure_schema(self): pass
    async def body_bytes(self, limit):
        return self.raw if len(self.raw) <= limit else None
    async def session(self, _data):
        return ("bi-" + self.actor, {"name": self.actor}) if self.actor else ("", None)
    async def audit(self, *args): self.audits.append(args)
    async def client_key(self): return self.client
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    def public_response(self, data, max_age):
        return {"status": 200, "data": data, "maxAge": max_age}
    def image_response(self, raw, media_type, max_age):
        return {"status": 200, "body": raw, "type": media_type, "maxAge": max_age}
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor
    @staticmethod
    def changes(cursor): return cursor.rowcount
    def ssh_helper_socket(self): return "/run/test.sock" if self.helper_available else ""
    def instance_install_url(self): return "https://forkmesh.com/install-instance.sh"
    def instance_source_url(self): return "https://app.forkmesh.com/forkmesh/forkmesh"
    def instance_source_ref(self): return "main"
    async def resolve_host(self, host):
        if host[0].isdigit():
            return [host]
        return self.addresses.get(host, [])
    def spawn(self, coroutine): self.spawned.append(coroutine)
    def run_spawned(self):
        while self.spawned:
            asyncio.run(self.spawned.pop(0))

    def ssh_helper(self, request):
        self.requests.append(json.loads(json.dumps(request)))

        async def events():
            if request.get("op") == "keyscan":
                yield {"keys": [dict(item) for item in self.keys]}
                return
            for event in self.install_events:
                yield dict(event)
        return events()


def form(**changes):
    body = {
        "projectName": "Acme Code",
        "host": "93.184.216.34",
        "port": 22,
        "sshUser": "root",
        "sshPassword": SSH_PASSWORD,
        "domain": "code.example.com",
        "adminUsername": "ada",
        "adminEmail": "ada@example.com",
        "adminPassword": ADMIN_PASSWORD,
        "ownerType": "org",
        "ownerName": "acme",
        "mailProvider": "sendgrid",
        "mailApiKey": MAIL_KEY,
    }
    body.update(changes)
    return body


def post(runtime, body, actor=""):
    raw = json.dumps(body).encode()
    return asyncio.run(instance_launch.handle(runtime.use(raw, actor), ("launch",)))


def status(runtime, launch_id):
    return asyncio.run(instance_launch.handle(
        runtime.use(b"", method="GET"), ("status", launch_id)))


def stored(runtime):
    rows = [dict(row) for row in runtime.db.execute(
        "SELECT * FROM instance_launches").fetchall()]
    return json.dumps([rows, runtime.audits], default=str)


def test_routes():
    assert instance_launch.match("launch") == ("launch",)
    assert instance_launch.match("launch/") == ("launch",)
    assert instance_launch.match("launches/abc") == ("status", "abc")
    assert instance_launch.match("launches") is None
    assert instance_launch.match("instances") == ("instances",)
    assert instance_launch.match("instances/abc/logo") == ("logo", "abc")
    assert instance_launch.match("instances/abc") is None
    assert instance_launch.match("other") is None


def test_sslip_names_follow_the_address():
    assert instance_launch.sslip_domain("93.184.216.34") == "93-184-216-34.sslip.io"
    assert instance_launch.sslip_domain("2001:db8::1").endswith(".sslip.io")


def test_validation_explains_each_field():
    cases = [
        (form(projectName="<b>"), "projectName"),
        (form(host="bad host!"), "host"),
        (form(port=70000), "port"),
        (form(sshPassword=""), "sshPassword"),
        (form(sshPrivateKey=KEY), "sshPassword"),
        (form(sshPassword="", sshPrivateKey="not a key"), "sshPrivateKey"),
        (form(hostKeyFingerprint="MD5:xx"), "hostKeyFingerprint"),
        (form(domain="not a domain"), "domain"),
        (form(domain="mine.forkmesh.com"), "domain"),
        (form(adminUsername="Ada!"), "adminUsername"),
        (form(adminUsername="forkmesh"), "adminUsername"),
        (form(adminEmail="nope"), "adminEmail"),
        (form(adminPassword="short"), "adminPassword"),
        (form(ownerType="team"), "ownerType"),
        (form(ownerName="ada"), "ownerName"),
        (form(ownerName="forkmesh"), "ownerName"),
        (form(mailProvider="postmark"), "mailProvider"),
        (form(mailApiKey='has "quotes"'), "mailApiKey"),
        (form(mailProvider="mailgun"), "mailgunDomain"),
        (form(mailFrom="nope"), "mailFrom"),
    ]
    for body, field in cases:
        result, problem = instance_launch.validate(body)
        assert result is None and problem[2] == field, (field, problem)


def test_user_owner_is_the_admin_and_no_mail_drops_mail_fields():
    result, problem = instance_launch.validate(form(
        ownerType="user", ownerName="ignored", mailProvider="none"))
    assert problem is None
    assert result["ownerName"] == "ada"
    assert result["mailApiKey"] == "" and result["mailFrom"] == ""
    result, _ = instance_launch.validate(form(
        domain="HTTPS://Code.Example.com/", sshPassword="",
        sshPrivateKey=KEY.replace("\n", "\r\n")))
    assert result["domain"] == "code.example.com"
    assert "\r" not in result["sshPrivateKey"]


def test_launch_pins_the_host_key_and_hands_secrets_only_to_the_helper():
    runtime = Runtime()
    answer = post(runtime, form())
    assert answer["status"] == 202, answer
    body = answer["data"]
    assert body["domain"] == "code.example.com"
    assert body["url"] == "https://code.example.com/acme/forkmesh"
    assert body["hostKey"] == {"type": "ssh-ed25519",
                               "fingerprint": fingerprint(ED_BLOB)}
    assert [step["id"] for step in body["steps"]] == list(instance_launch.STEP_IDS)
    keyscan, install = runtime.requests
    assert keyscan == {"op": "keyscan", "address": "93.184.216.34", "port": 22}
    assert install["op"] == "instance"
    assert install["hostKey"] == {"type": "ssh-ed25519", "key": ED_BLOB}
    assert install["password"] == SSH_PASSWORD
    assert install["installUrl"] == "https://forkmesh.com/install-instance.sh"
    config = install["config"]
    assert config["adminPassword"] == ADMIN_PASSWORD
    assert config["mailApiKey"] == MAIL_KEY
    assert config["mailFrom"] == "no-reply@code.example.com"
    assert config["ownerName"] == "acme" and config["domainProvided"] is True
    assert config["sourceUrl"] == "https://app.forkmesh.com/forkmesh/forkmesh"
    dump = stored(runtime)
    for secret in (SSH_PASSWORD, ADMIN_PASSWORD, MAIL_KEY):
        assert secret not in dump
        assert secret not in json.dumps(body)
    assert runtime.audits == [("anonymous", "instance.launch.start",
                               "code.example.com")]


def test_no_domain_starts_on_sslip_and_a_wrong_fingerprint_stops():
    runtime = Runtime()
    answer = post(runtime, form(domain=""))
    assert answer["data"]["domain"] == "93-184-216-34.sslip.io"
    assert runtime.requests[-1]["config"]["domainProvided"] is False
    runtime = Runtime()
    answer = post(runtime, form(hostKeyFingerprint=fingerprint(RSA_BLOB)))
    assert answer["data"]["hostKey"]["type"] == "ssh-rsa"
    wrong = "SHA256:" + "A" * 43
    answer = post(Runtime(), form(hostKeyFingerprint=wrong))
    assert answer["status"] == 400
    assert answer["data"]["error"] == "fingerprint_mismatch"


def test_private_addresses_unreachable_servers_and_limits():
    runtime = Runtime()
    for host in ("lan.example.com", "mixed.example.com", "10.77.0.2",
                 "127.0.0.1", "nowhere.example.com"):
        answer = post(runtime, form(host=host))
        assert answer["status"] == 400 and answer["data"]["field"] == "host", host
    runtime.keys = []
    answer = post(runtime, form())
    assert answer["status"] == 502
    assert answer["data"]["error"] == "ssh_unreachable"
    runtime = Runtime()
    runtime.helper_available = False
    assert post(runtime, form())["status"] == 503
    runtime = Runtime()
    for _ in range(instance_launch.MAX_LAUNCHES_PER_HOUR):
        assert post(runtime, form())["status"] == 202
    assert post(runtime, form())["status"] == 429
    runtime.client = "client-b"
    assert post(runtime, form())["status"] == 202
    runtime.client = "client-c"
    assert post(runtime, form())["status"] == 503


def test_progress_lines_become_steps_and_the_log_hides_secrets():
    runtime = Runtime()
    runtime.install_events = [
        {"out": "Connected to 93.184.216.34 as root; host key ssh-ed25519 verified."},
        {"out": 'FMI {"step":"system","state":"running"}'},
        {"out": "Debian GNU/Linux 13, x86_64"},
        {"out": 'FMI {"step":"packages","state":"running"}'},
        {"out": "echo " + ADMIN_PASSWORD},
        {"out": "key " + MAIL_KEY},
        {"out": 'FMI {"step":"repo","state":"running"}'},
        {"out": 'FMI {"done":true,"url":"https://code.example.com/acme/forkmesh"}'},
        {"exit": 0},
    ]
    launch_id = post(runtime, form())["data"]["id"]
    running = status(runtime, launch_id)["data"]["launch"]
    assert running["state"] == "running"
    runtime.run_spawned()
    launch = status(runtime, launch_id)["data"]["launch"]
    assert launch["state"] == "succeeded"
    assert launch["url"] == "https://code.example.com/acme/forkmesh"
    assert all(step["state"] == "done" for step in launch["steps"])
    assert "Debian GNU/Linux 13" in launch["log"]
    assert "FMI " not in launch["log"]
    assert ADMIN_PASSWORD not in launch["log"] and MAIL_KEY not in launch["log"]
    assert "[hidden]" in launch["log"]
    assert launch["admin"] == "ada" and launch["owner"] == "acme"


def test_a_failed_installer_marks_the_running_step_failed():
    runtime = Runtime()
    runtime.install_events = [
        {"out": 'FMI {"step":"system","state":"running"}'},
        {"out": 'FMI {"step":"packages","state":"running"}'},
        {"out": "Error: apt broke"},
        {"exit": 1},
    ]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    launch = status(runtime, launch_id)["data"]["launch"]
    assert launch["state"] == "failed"
    states = {step["id"]: step["state"] for step in launch["steps"]}
    assert states["system"] == "done" and states["packages"] == "failed"
    assert states["repo"] == "pending"
    assert "status 1" in launch["message"]
    assert launch["url"] == ""


def test_an_exit_without_the_done_line_is_not_a_success():
    runtime = Runtime()
    runtime.install_events = [{"out": "partial"}, {"exit": 0}]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    assert status(runtime, launch_id)["data"]["launch"]["state"] == "failed"


def test_redirects_stay_on_the_instance_domain():
    assert instance_launch.safe_redirect(
        "https://code.example.com/acme/forkmesh", "code.example.com")
    assert not instance_launch.safe_redirect(
        "https://evil.example/acme", "code.example.com")
    assert not instance_launch.safe_redirect(
        "https://code.example.com/<script>", "code.example.com")
    runtime = Runtime()
    runtime.install_events = [
        {"out": 'FMI {"done":true,"url":"https://evil.example/x"}'},
        {"exit": 0},
    ]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    launch = status(runtime, launch_id)["data"]["launch"]
    assert launch["url"] == "https://code.example.com/acme/forkmesh"


def test_status_needs_a_real_id_and_turns_stale_runs_failed():
    runtime = Runtime()
    assert status(runtime, "nope")["status"] == 404
    assert status(runtime, "f" * 32)["status"] == 404
    launch_id = post(runtime, form())["data"]["id"]
    runtime.clock += instance_launch.RUN_STALE_MS + 1
    launch = status(runtime, launch_id)["data"]["launch"]
    assert launch["state"] == "failed"
    wrong = asyncio.run(instance_launch.handle(
        runtime.use(b"{}", method="GET"), ("launch",)))
    assert wrong["status"] == 405


def test_the_log_reaches_the_page_while_the_server_is_quiet(monkeypatch):
    monkeypatch.setattr(instance_launch, "FLUSH_MS", 20)
    runtime = Runtime()
    seen = []

    def live_log():
        return runtime.db.execute("SELECT log FROM instance_launches").fetchone()[0]

    def helper(request):
        async def events():
            if request.get("op") == "keyscan":
                yield {"keys": [dict(item) for item in runtime.keys]}
                return
            yield {"out": 'FMI {"step":"build","state":"running"}'}
            yield {"out": "build: tools/build_worker_python.py"}
            await asyncio.sleep(0.05)
            runtime.clock += 1000
            await asyncio.sleep(0.2)
            seen.append(live_log())
            yield {"out": 'FMI {"done":true,"url":"https://code.example.com/acme/forkmesh"}'}
            yield {"exit": 0}
        return events()

    runtime.ssh_helper = helper
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    assert seen and "build_worker_python" in seen[0]
    assert status(runtime, launch_id)["data"]["launch"]["state"] == "succeeded"


def test_a_dropped_connection_says_the_server_keeps_going():
    runtime = Runtime()
    runtime.install_events = [
        {"out": 'FMI {"step":"build","state":"running"}'},
        {"out": "build: tools/build_site_assets.py app"},
        {"exit": 255, "error": "SSH could not sign in or the host key did not match."},
    ]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    launch = status(runtime, launch_id)["data"]["launch"]
    assert launch["state"] == "failed"
    assert "keeps running" in launch["message"]
    assert instance_launch.SETUP_UNIT in launch["message"]
    assert {step["id"]: step["state"] for step in launch["steps"]}["build"] == "failed"

    runtime = Runtime()
    runtime.install_events = [{"out": 'FMI {"step":"system","state":"running"}'}]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    assert "keeps running" in status(runtime, launch_id)["data"]["launch"]["message"]

    runtime = Runtime()
    runtime.install_events = [{"exit": 255, "error": "SSH could not sign in or the host key did not match."}]
    launch_id = post(runtime, form())["data"]["id"]
    runtime.run_spawned()
    assert "could not sign in" in status(runtime, launch_id)["data"]["launch"]["message"]


def test_status_carries_what_the_retry_form_needs():
    runtime = Runtime()
    launch_id = post(runtime, form(port=2222))["data"]["id"]
    launch = status(runtime, launch_id)["data"]["launch"]
    assert (launch["projectName"], launch["address"], launch["port"]) == ("Acme Code", "93.184.216.34", 2222)
    assert (launch["domain"], launch["admin"], launch["ownerType"], launch["owner"]) == (
        "code.example.com", "ada", "org", "acme")
    runtime.clock += instance_launch.RUN_STALE_MS + 1
    assert "keeps running" in status(runtime, launch_id)["data"]["launch"]["message"]


PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24
SVG = (b'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1 1">'
       b'<rect width="1" height="1"/></svg>')


def data_url(raw, kind="image/png"):
    return "data:%s;base64,%s" % (kind, base64.b64encode(raw).decode())


def test_a_logo_is_checked_by_its_bytes():
    result, problem = instance_launch.validate(form(logo=data_url(PNG, "image/jpeg")))
    assert problem is None and result["logo"] == data_url(PNG)
    result, problem = instance_launch.validate(form(logo=data_url(SVG, "image/svg+xml")))
    assert problem is None and result["logo"] == data_url(SVG, "image/svg+xml")
    assert instance_launch.validate(form())[0]["logo"] == ""
    for bad in ("https://example.com/logo.png", data_url(b"GIF89a" + b"\x00" * 20),
                data_url(b'<svg onload="alert(1)"></svg>', "image/svg+xml"),
                data_url(b"<svg><script>alert(1)</script></svg>", "image/svg+xml"),
                data_url(b'<svg><a href="javascript:alert(1)">x</a></svg>', "image/svg+xml"),
                data_url(PNG + b"\x00" * instance_launch.MAX_LOGO_BYTES)):
        result, problem = instance_launch.validate(form(logo=bad))
        assert result is None and problem[0] == "invalid_logo" and problem[2] == "logo", bad


def test_a_large_logo_reaches_the_installer_and_is_not_stored():
    runtime = Runtime()
    logo = data_url(PNG + b"\x00" * (instance_launch.MAX_LOGO_BYTES - len(PNG)))
    answer = post(runtime, form(logo=logo))
    assert answer["status"] == 202, answer
    assert runtime.requests[1]["config"]["logo"] == logo
    assert logo[30:90] not in stored(runtime)


def test_a_theme_preset_is_checked_and_reaches_the_installer():
    for name in instance_theme.PRESETS:
        result, problem = instance_launch.validate(form(theme=name))
        assert problem is None and result["theme"] == name
    assert instance_launch.validate(form())[0]["theme"] == ""
    for bad in ("blue", "../crimson", "x" * 60, "{}"):
        result, problem = instance_launch.validate(form(theme=bad))
        assert result is None and problem[0] == "invalid_theme" and problem[2] == "theme", bad
    assert '"theme": form["theme"],' in Path(instance_launch.__file__).read_text(encoding="utf-8")


def test_a_full_colour_theme_is_cleaned_and_reaches_the_installer():
    colours = {"dark": {"primary": "#F97316", "background": "#0c0a09"},
               "light": {"primary": "#ea580c"}}
    result, problem = instance_launch.validate(form(theme=colours))
    assert problem is None
    assert result["theme"] == instance_theme.clean(colours)
    assert result["theme"]["dark"]["primary"] == "#f97316"
    assert instance_launch.validate(form(theme={}))[0]["theme"] == ""
    for bad in ({"sepia": {"primary": "#000000"}}, {"dark": {"primary": "red"}},
                {"dark": {"cta": "#000000"}}, {"dark": ["#000000"]}, ["dark"], 7,
                {"dark": {name: "#000000" for name in instance_theme.TOKENS},
                 "light": {"primary": "url(x)"}}):
        result, problem = instance_launch.validate(form(theme=bad))
        assert result is None and problem[0] == "invalid_theme" and problem[2] == "theme", bad
    runtime = Runtime()
    answer = post(runtime, form(theme=colours))
    assert answer["status"] == 202, answer
    assert runtime.requests[1]["config"]["theme"] == instance_theme.clean(colours)


OWNER_PASSWORD = "grace-password-33"  # forkmesh-secret-scan:ignore-line


def owner(**changes):
    item = {"username": "Grace", "email": "Grace@Example.com", "password": OWNER_PASSWORD}
    item.update(changes)
    return item


def test_other_owners_are_checked_one_by_one():
    result, problem = instance_launch.validate(form(owners=[owner(), owner(
        username="linus", email="linus@example.com")]))
    assert problem is None
    assert result["owners"] == [
        {"username": "grace", "email": "grace@example.com", "password": OWNER_PASSWORD},
        {"username": "linus", "email": "linus@example.com", "password": OWNER_PASSWORD}]
    assert instance_launch.validate(form())[0]["owners"] == []
    assert instance_launch.validate(form(owners=None))[0]["owners"] == []
    cases = [
        (form(owners="grace"), "invalid_owners", "owners"),
        (form(owners=["grace"]), "invalid_owners", "owners"),
        (form(owners=[owner()] * 21), "invalid_owners", "owners"),
        (form(owners=[owner()], ownerType="user"), "owners_need_org", "ownerType"),
        (form(owners=[owner(username="Grace!")]), "invalid_owner_username", "owners"),
        (form(owners=[owner(username="forkmesh")]), "invalid_owner_username", "owners"),
        (form(owners=[owner(username="ada")]), "duplicate_owner", "owners"),
        (form(owners=[owner(username="acme")]), "duplicate_owner", "owners"),
        (form(owners=[owner(), owner(email="other@example.com")]), "duplicate_owner", "owners"),
        (form(owners=[owner(email="nope")]), "invalid_owner_email", "owners"),
        (form(owners=[owner(email="ADA@example.com")]), "invalid_owner_email", "owners"),
        (form(owners=[owner(), owner(username="linus")]), "invalid_owner_email", "owners"),
        (form(owners=[owner(password="short")]), "weak_owner_password", "owners"),
        (form(owners=[owner(password=None)]), "weak_owner_password", "owners"),
    ]
    for body, code, field in cases:
        result, problem = instance_launch.validate(body)
        assert result is None and problem[0] == code and problem[2] == field, (code, problem)


def test_other_owners_reach_the_installer_and_their_passwords_stay_hidden():
    runtime = Runtime()
    runtime.install_events = [{"out": "owner password is %s" % OWNER_PASSWORD}, {"exit": 1}]
    answer = post(runtime, form(owners=[owner()]))
    assert answer["status"] == 202, answer
    assert runtime.requests[1]["config"]["owners"] == [
        {"username": "grace", "email": "grace@example.com", "password": OWNER_PASSWORD}]
    runtime.run_spawned()
    dump = stored(runtime)
    assert OWNER_PASSWORD not in dump and OWNER_PASSWORD not in json.dumps(answer)


def test_the_admin_console_path_is_made_checked_handed_over_and_hidden():
    import admin_path
    runtime = Runtime()
    chosen = "/manage-x7k2q9"
    runtime.install_events = [
        {"out": "Admin console: https://code.example.com%s" % chosen},
        {"out": 'FMI {"done":true,"url":"https://code.example.com/acme/forkmesh"}'},
        {"exit": 0},
    ]
    answer = post(runtime, form(adminPath=chosen))
    assert answer["status"] == 202
    assert answer["data"]["adminPath"] == chosen
    assert runtime.requests[1]["config"]["adminPath"] == chosen
    runtime.run_spawned()
    launch = status(runtime, answer["data"]["id"])["data"]["launch"]
    assert chosen not in json.dumps(launch)
    assert "Admin console: https://code.example.com[hidden]" in launch["log"]
    assert chosen not in stored(runtime)

    made = post(Runtime(), form())["data"]["adminPath"]
    assert admin_path.GENERATED_RE.fullmatch(made)
    assert post(Runtime(), form())["data"]["adminPath"] != made

    for bad in ("manage-x7k2q9", "/short", "/admin", "/dashboard", "/a/b/c/d/e/f"):
        refused = post(Runtime(), form(adminPath=bad))
        assert refused["status"] == 400, bad
        assert refused["data"]["error"] == "invalid_admin_path"
        assert refused["data"]["field"] == "adminPath"
