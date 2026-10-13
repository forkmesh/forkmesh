import http.client
import http.server
import json
import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

import caddy_routes
import render
import request_rules
from test_caddy import CADDY, FLEET, free_port, make_dists, needs_caddy

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app" / "host"))

from forkmesh_host.request_policy import Rules  # noqa: E402

KEYWORDS = ["ZxqCrawler", "fake.agent(v2)", "odd`tick", "dollar$sign",
            "brace{x}", "plus+star*", "back\\slash", "pipe|bar", "two words"]


def export(rows):
    return Rules.from_rows(rows).export()


def keyword_rows(words):
    return [{"match_type": "user_agent_keyword", "pattern": w} for w in words]


def caddy_pattern(text):
    match = re.search(r"header_regexp request_rules_agent User-Agent `([^`]*)`",
                      text)
    return match.group(1)


def test_app_export_round_trips_through_the_renderer():
    rows = keyword_rows(KEYWORDS) + [
        {"match_type": "ip", "pattern": "198.51.100.7"},
        {"match_type": "cidr", "pattern": "203.0.113.0/24"},
        {"match_type": "ip_range", "pattern": "192.0.2.10 - 192.0.2.20"},
        {"match_type": "cidr", "pattern": "2001:db8::/32"},
    ]
    keywords, nets, skipped = request_rules.parse(export(rows))
    assert keywords == sorted(k.lower() for k in KEYWORDS)
    assert skipped == 0
    assert "198.51.100.7/32" in nets and "203.0.113.0/24" in nets
    assert "2001:db8::/32" in nets
    assert {"192.0.2.10/31", "192.0.2.12/30", "192.0.2.16/30",
            "192.0.2.20/32"} <= set(nets)


def test_keyword_regex_matches_like_the_host():
    rules = Rules.from_rows(keyword_rows(KEYWORDS))
    text = request_rules.render(*request_rules.parse(rules.export())[:2])
    pattern = re.compile(caddy_pattern(text))
    agents = ["Mozilla/5.0 zxqcrawler/1.0", "ZXQCRAWLER", "x FAKE.AGENT(V2) y",
              "fakeXagent(v2)", "odd`tick", "a DOLLAR$SIGN", "brace{X}",
              "plus+star*!", "back\\slash", "pipe|bar", "TWO WORDS here",
              "twowords", "Mozilla/5.0 Firefox/140.0", "", "brace{y}",
              "plusstar", "pipe"]
    for agent in agents:
        assert bool(pattern.search(agent)) == rules.agent_blocked(agent), agent


def test_render_escapes_caddyfile_specials():
    text = request_rules.render(["odd`tick", "brace{$env}", "x{args[0]}"], [])
    body = caddy_pattern(text)
    assert "`" not in body and "{$" not in body and "{args[" not in body
    assert "\\x60" in body


def test_non_ascii_and_oversized_keywords_are_left_to_the_host():
    data = json.dumps({"version": 1, "agents": ["zxqé", "a" * 300, " ",
                                                 "fine\tTab", "ok"],
                       "networks": ["not-a-network"], "ranges": [["b", "a"]]})
    keywords, nets, skipped = request_rules.parse(data)
    assert keywords == ["ok"] and nets == [] and skipped == 6


def test_bad_exports_are_refused():
    for text in ("{", "[]", json.dumps({"version": 2, "agents": ["x"]})):
        with pytest.raises(request_rules.RulesError):
            request_rules.parse(text)
    many = json.dumps({"version": 1, "agents": ["zxq%d" % i for i in range(1001)]})
    with pytest.raises(request_rules.RulesError):
        request_rules.parse(many)


def test_empty_rules_render_only_the_marker():
    assert request_rules.render([], []) == request_rules.MARKER + "\n"


def test_main_writes_once_and_reloads_only_on_change(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(request_rules.subprocess, "run",
                        lambda argv, check: calls.append(argv) or
                        subprocess.CompletedProcess(argv, 0))
    rules = tmp_path / "app" / "request-rules.json"
    out = tmp_path / "caddy" / "request-rules.caddy"
    argv = ["--rules", str(rules), "--out", str(out), "--reload"]
    assert request_rules.main(argv) == 0
    assert out.read_text() == request_rules.MARKER + "\n"
    assert calls == [request_rules.RELOAD]
    rules.parent.mkdir()
    rules.write_text(export(keyword_rows(["zxqcrawler"])))
    assert request_rules.main(argv) == 0
    assert "zxqcrawler" in out.read_text()
    assert oct(out.stat().st_mode & 0o777) == "0o644"
    assert len(calls) == 2
    assert request_rules.main(argv) == 0
    assert len(calls) == 2
    rules.write_text("{broken")
    assert request_rules.main(argv) == 1
    assert "zxqcrawler" in out.read_text()
    assert len(calls) == 2


def test_rendered_core_installs_the_renderer_and_units(tmp_path):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    files, manifest = render.render_core(fleet, "n1")
    assert "fmnext-request-rules.path" in manifest["units"]
    assert "fmnext-request-rules.service" in manifest["units"]
    assert files["/opt/forkmesh-next/bin/fmnext-request-rules"][1] == 0o755
    unit = files["/etc/systemd/system/fmnext-request-rules.path"][0]
    assert "PathChanged=/var/lib/forkmesh-next/app/request-rules.json" in unit
    service = files["/etc/systemd/system/fmnext-request-rules.service"][0]
    assert ("--out /etc/forkmesh-next/caddy/request-rules.caddy --reload"
            in service)
    env = files["/etc/forkmesh-next/app.env"][0]
    assert ("FORKMESH_REQUEST_RULES_FILE=/var/lib/forkmesh-next/app/"
            "request-rules.json") in env.splitlines()
    caddyfile = files["/etc/caddy/Caddyfile"][0]
    for host in ("app.next.forkmesh.com", "world.next.forkmesh.com",
                 "n1.next.forkmesh.com"):
        block = caddyfile[caddyfile.index("\n%s {" % host):]
        block = block[:block.index("\n}\n")]
        lines = [line.strip() for line in block.splitlines()]
        imports = [line for line in lines if line.startswith("import ")]
        assert any(line.endswith("/request-rules*.caddy") for line in imports)
        first_handle = next(i for i, line in enumerate(lines)
                            if line.startswith(("handle", "import site_common",
                                                "import /etc/forkmesh-next/caddy/git")))
        rules_at = next(i for i, line in enumerate(lines)
                        if line.endswith("/request-rules*.caddy"))
        assert rules_at < first_handle
    ids = json.loads(files["/etc/forkmesh-next/ids.json"][0]) if (
        "/etc/forkmesh-next/ids.json" in files) else None
    assert "/etc/forkmesh-next/caddy/request-rules.caddy" in render.IDS_EXCLUDE
    if ids is not None:
        assert "/etc/forkmesh-next/caddy/request-rules.caddy" in json.dumps(ids)


def test_production_legacy_hosts_import_the_rules(tmp_path):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    fleet["production"] = dict(render.PRODUCTION_HOSTS)
    caddyfile = render.caddyfile(fleet, fleet["nodes"][0],
                                 render.fleet_paths(fleet))
    for host in render.tls_hosts(fleet):
        block = caddyfile[caddyfile.index("\n%s {" % host):]
        block = block[:block.index("\n}\n")]
        assert "request-rules*.caddy" in block, host
    assert "header_up Host app.forkmesh.com" in caddyfile


class FakeApp(http.server.BaseHTTPRequestHandler):
    checks = []

    def log_message(self, *args):
        pass

    def _answer(self):
        length = int(self.headers.get("content-length") or 0)
        body = self.rfile.read(length) if length else b""
        headers = {k.lower(): v for k, v in self.headers.items()}
        if self.path == render.REQUEST_CHECK_PATH:
            type(self).checks.append(headers)
            denied = "deny" in headers.get("user-agent", "").lower()
            self.send_response(403 if denied else 204)
            data = b"Forbidden\n" if denied else b""
        else:
            self.send_response(200)
            data = json.dumps({"tag": "app", "path": self.path,
                               "body": body.decode(), "headers": headers}).encode()
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    do_GET = do_POST = do_HEAD = _answer


def serve(handler):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


@pytest.fixture
def rules_live(tmp_path):
    handler = type("App", (FakeApp,), {"checks": []})
    app = serve(handler)
    gateway = serve(type("Gateway", (FakeApp,), {"checks": []}))
    http_port = free_port()
    current = tmp_path / "current"
    app_dist, world_dist = make_dists(current)
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    caddy_dir = tmp_path / "etc/forkmesh-next/caddy"
    fleet["paths"] = {"etc": str(tmp_path / "etc/forkmesh-next"),
                      "current": str(current), "tls": str(tmp_path / "tls")}
    fleet["app"]["port"] = app.server_address[1]
    fleet["app"]["gateway_port"] = gateway.server_address[1]
    files, _ = render.render_core(fleet, "n1")
    caddy_dir.mkdir(parents=True)
    (caddy_dir / "app-routes.caddy").write_text(
        caddy_routes.generate("app", REPO, dist=app_dist, root=str(app_dist)))
    (caddy_dir / "world-routes.caddy").write_text(caddy_routes.generate(
        "world", REPO, dist=world_dist, root=str(world_dist),
        app_root=str(app_dist), app_dist=app_dist))
    for host in (fleet["app_host"], fleet["world_host"]):
        (caddy_dir / ("tls-%s.caddy" % host)).write_text("# test\n")
    (caddy_dir / "acme-http.caddy").write_text("# test\n")
    caddyfile = files["/etc/caddy/Caddyfile"][0]
    body = caddyfile[caddyfile.index("(client_headers)"):]
    for host in ("app.next.forkmesh.com", "world.next.forkmesh.com",
                 "n1.next.forkmesh.com"):
        body = body.replace("\n%s {" % host, "\nhttp://%s:%d {" % (host, http_port))
    admin = tmp_path / "admin.sock"
    config = tmp_path / "Caddyfile.live"
    config.write_text("{\n\tadmin unix/%s\n\tauto_https off\n\thttp_port %d\n}\n\n%s"
                      % (admin, http_port, body))
    rules_file = tmp_path / "request-rules.json"
    out = caddy_dir / "request-rules.caddy"
    process = None

    def apply(rows):
        rules_file.write_text(export(rows))
        assert request_rules.main(["--rules", str(rules_file), "--out", str(out)]) == 0
        if process is not None:
            done = subprocess.run([CADDY, "reload", "--config", str(config),
                                   "--adapter", "caddyfile", "--address",
                                   "unix/%s" % admin],
                                  capture_output=True, text=True, check=False)
            assert done.returncode == 0, done.stderr

    def request(host, path, agent="Mozilla/5.0 Firefox/140.0", method="GET",
                body=None):
        connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=10)
        connection.request(method, path, body=body,
                           headers={"Host": host, "User-Agent": agent})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, data

    apply([])
    process = subprocess.Popen([CADDY, "run", "--config", str(config),
                                "--adapter", "caddyfile"],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", http_port), timeout=0.2).close()
            break
        except OSError:
            if process.poll() is not None:
                break
            time.sleep(0.1)
    if process.poll() is not None:
        pytest.fail("caddy exited: %s" % process.stdout.read().decode()[-2000:])
    yield apply, request, handler
    process.terminate()
    process.wait(timeout=10)
    app.shutdown()
    gateway.shutdown()


APP = "app.next.forkmesh.com"
WORLD = "world.next.forkmesh.com"
NODE = "n1.next.forkmesh.com"


@needs_caddy
def test_live_keyword_rules_cover_static_routes_and_reload(rules_live):
    apply, request, app = rules_live
    agent = "Mozilla/5.0 (compatible; ZXQCRAWLER/1.0)"
    assert request(APP, "/login", agent) == (200, b"LOGIN PAGE")
    assert request(WORLD, "/", agent)[1] == b"WORLD INDEX"
    apply(keyword_rows(["zxqcrawler", "fake.agent(v2)"]))
    for host, path in ((APP, "/login"), (APP, "/api/x"), (WORLD, "/"),
                       (WORLD, "/world/world.js"), (NODE, "/owner/repo/info/refs"),
                       (APP, "/api/chat/agent-guestbook"), (APP, "/api/agent-notes")):
        assert request(host, path, agent) == (403, b"Forbidden"), (host, path)
    assert request(APP, "/login", "x FAKE.AGENT(V2) y")[0] == 403
    assert request(APP, "/login", "fakeXagent(v2)") == (200, b"LOGIN PAGE")
    assert request(APP, "/login") == (200, b"LOGIN PAGE")
    for host, path in ((APP, "/health"), (APP, "/readyz"),
                       (APP, "/api/fleet/status"), (NODE, "/readyz")):
        status, data = request(host, path, agent)
        assert status == 200 and json.loads(data)["path"] == path
    assert app.checks == []
    apply([])
    assert request(APP, "/login", agent) == (200, b"LOGIN PAGE")


@needs_caddy
def test_live_google_named_agents_are_checked_by_the_app(rules_live):
    apply, request, app = rules_live
    apply(keyword_rows(["zxqbot"]))
    good = "Mozilla/5.0 (compatible; Googlebot/2.1; zxqbot)"
    assert request(APP, "/login", good) == (200, b"LOGIN PAGE")
    assert request(WORLD, "/", good)[1] == b"WORLD INDEX"
    check = app.checks[0]
    assert check["x-forwarded-uri"] == "/login"
    assert check["x-forwarded-method"] == "GET"
    assert check["cf-connecting-ip"] == "127.0.0.1"
    assert check["host"] == APP
    status, data = request(APP, "/api/x", good, method="POST", body=b"payload")
    assert status == 200 and json.loads(data)["body"] == "payload"
    fake = "Mozilla/5.0 (compatible; Googlebot/2.1; zxqbot; deny)"
    assert request(APP, "/login", fake) == (403, b"Forbidden\n")
    assert request(NODE, "/owner/repo/info/refs", fake)[0] == 403
    before = len(app.checks)
    assert request(APP, "/login", "Googlebot/2.1") == (200, b"LOGIN PAGE")
    assert len(app.checks) == before


@needs_caddy
def test_live_chatgpt_agents_pass_keyword_rules(rules_live):
    apply, request, app = rules_live
    apply(keyword_rows(["bot"]))
    user = "Mozilla/5.0 (compatible; ChatGPT-User/1.0; +https://openai.com/bot)"
    search = "Mozilla/5.0 (compatible; OAI-SearchBot/1.0; +https://openai.com/searchbot)"
    for agent in (user, search, "Googlebot/2.1 ChatGPT-User/1.0 deny"):
        assert request(APP, "/login", agent) == (200, b"LOGIN PAGE"), agent
        assert request(WORLD, "/", agent)[1] == b"WORLD INDEX"
    assert app.checks == []
    assert request(APP, "/login", "GPTBot/1.1")[0] == 403


@needs_caddy
def test_live_address_rules(rules_live):
    apply, request, app = rules_live
    apply([{"match_type": "ip_range", "pattern": "127.0.0.1 - 127.0.0.1"}])
    for host, path in ((APP, "/login"), (WORLD, "/"), (NODE, "/x/y/info/refs")):
        assert request(host, path) == (403, b"Forbidden"), (host, path)
    assert request(APP, "/health")[0] == 200
    apply([{"match_type": "cidr", "pattern": "10.0.0.0/8"}])
    assert request(APP, "/login") == (200, b"LOGIN PAGE")
