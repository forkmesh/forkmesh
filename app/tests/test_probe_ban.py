import ast
import importlib.util
import json
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse

import pytest

from test_not_found_log import CSRF, NOW, FakeRequest, Harness, _run

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
HOUR = 3_600_000
DAY = 24 * HOUR
SCANNER = "93.184.216.34"
OTHER = "93.184.216.35"
THIRD = "93.184.216.36"
CLIENT = {"ip": SCANNER, "country": "NL", "ua": "Mozilla/5.0 zgrab/0.x"}

SCANNER_PATHS = {
    "/.env": "secret",
    "/.env.production": "secret",
    "/.env~": "secret",
    "/%2f.env.local": "secret",
    "/api/.env": "secret",
    "/api/staging/.env": "secret",
    "/laravel/.env": "secret",
    "/sendgrid.env": "secret",
    "/env.txt": "secret",
    "/runtime-env.js": "secret",
    "/.git/config": "secret",
    "/.git-credentials": "secret",
    "/.aws/credentials": "secret",
    "/.s3cfg": "secret",
    "/.npmrc": "secret",
    "/.vscode/sftp.json": "secret",
    "/credentials.json": "secret",
    "/gcp-credentials.json": "secret",
    "/firebase-adminsdk.json": "secret",
    "/.mcp.json": "secret",
    "/claude_desktop_config.json": "secret",
    "/wp-config.php.bak": "secret",
    "/%20../web-inf/web.xml": "secret",
    "/id_rsa": "secret",
    "/phpinfo.php": "script",
    "/api/phpinfo.php": "script",
    "/xmlrpc.php": "script",
    "/_internal/api/setup.php": "script",
    "/cgi-bin/supervisor/Factory.cgi": "script",
    "/CFIDE/main/ide.cfm": "script",
    "/login.action": "script",
    "/npm-pwg/..;/axis2-AWC/services/listServices": "script",
    "/phpinfo": "script",
    "/wp-login": "script",
    "//wp/wp-includes/wlwmanifest.xml": "script",
    "/wordpress/": "script",
    "/blog/wp-includes/wlwmanifest.xml": "cms",
    "/actuator/health/db": "cms",
    "/solr/admin/cores": "cms",
    "/server-status": "cms",
    "/database.sql": "secret",
    "/backup.zip": "backup",
    "/storage/logs/laravel.log": "backup",
    "/docker-compose.yml~": "backup",
    "/config.json": "config",
    "/appsettings.Production.json": "config",
    "/docker-compose.yml": "config",
    "/.gitlab-ci.yml": "config",
    "/Dockerfile": "config",
    "/composer.lock": "config",
    "/runtime-config.js": "config",
    "/AGENTS.md": "config",
    "/env": "config",
    "/MAILGUN_API_KEY": "secret",
    "/MAIL_PASSWORD": "secret",
    "/ses-smtp-credentials": "secret",
    "/aws_keys": "secret",
    "/service-accounts": "secret",
    "/openai-key%2etxt": "secret",
    "/github_token.txt": "secret",
    "/creds%2ejson": "secret",
    "/aws-ses%2ejson": "secret",
    "/planetscale.json": "secret",
    "/latest/meta-data/iam/security-credentials/": "secret",
    "/computeMetadata/v1/instance/service-accounts/default/token": "secret",
    "/metadata/identity/oauth2/token": "secret",
    "/v1/sys/seal-status": "secret",
    "/config/initializers/stripe%2erb": "secret",
    "/app/etc/local%2exml": "secret",
    "/status%252ephp": "script",
    "/config%252ephp%252ebak": "script",
    "/%252e%252e/": "script",
    "/%c0": "script",
    "/C:/Windows/System32/drivers/etc/hosts": "script",
    "/api/index.php/v1/config/application": "script",
    "/phpinfo%2ehtml": "script",
    "/api/actuator/health/kafka": "cms",
    "/debug/pprof/heap": "cms",
    "/v1/catalog/services": "cms",
    "/v3/kv/range": "cms",
    "/v1.40/containers/json": "cms",
    "/_cat/": "cms",
    "/_search": "cms",
    "/grafana/api/datasources": "cms",
    "/jenkins/api/json": "cms",
    "/scriptText": "cms",
    "/api/0/organizations/": "cms",
    "/rails/info/properties": "cms",
    "/_debugbar/": "cms",
    "/administrator/": "cms",
    "/functionRouter": "cms",
    "/storage/logs/laravel%2elog%2e1": "script",
    "/storage/logs/laravel.log.1": "backup",
    "/var/log/mail%2eerr": "script",
    "/var/log/mail.err": "backup",
    "/package%2ejson%2eswn": "script",
    "/package.json.swn": "backup",
    "/dump%2ecsv": "script",
    "/dump.csv": "backup",
    "/go.sum": "config",
    "/requirements.txt": "config",
    "/manage%2epy": "script",
    "/manage.py": "config",
    "/node_modules/": "config",
    "/swagger-ui%2ehtml": "script",
    "/swagger-ui.html": "config",
    "/v2/api-docs": "config",
    "/MAIL_HOST": "secret",
    "/SENDGRID_TEMPLATE_ID": "secret",
    "/internal/v2/config/mps_secret/ADM_SESSIONID": "secret",
    "/_environment": "secret",
    "/_netrc": "secret",
    "/psql_history": "secret",
    "/terraform.tfvars": "secret",
    "/GoogleService-Info.plist": "secret",
    "/snowflake.json": "secret",
    "/sftp-config.json": "secret",
    "/api/v1/.git/config": "secret",
    "/api/v1/secrets": "secret",
    "/dump%2esqlite": "secret",
    "/CHANGELOG%2emd": "script",
    "/favicon%2eico": "script",
    "/%2eeslintrc": "script",
    "/index.php.txt": "script",
    "/index.do": "script",
    "/wp-json/wp/v2/users": "script",
    "/phpMyAdmin/": "script",
    "/_profiler/router/latest": "script",
    "/metabase/api/session/properties": "cms",
    "/api/session/properties": "cms",
    "/bin/get/Main/SolrSearch": "cms",
    "/rest/api/1.0/application-properties": "cms",
    "/media/system/js/core.js": "cms",
    "/api/users-permissions/roles": "cms",
    "/api/v1/bedrock-runtime": "cms",
    "/v1/models": "cms",
    "/manager/html": "cms",
    "/elmah/": "cms",
    "/backup.tbz2": "backup",
    "/Jenkinsfile": "config",
    "/api/graphql": "config",
    "/swagger-resources/configuration/ui": "config",
    "/node_modules/mqtt/test/helpers/": "config",
    "/vercel.json": "config",
    "/kafka/": "recon",
    "/mysql": "recon",
    "/console": "recon",
    "/proxy": "recon",
    "/secrets": "recon",
    "/mailgun": "recon",
    "/whoami": "recon",
    "/_next/static/chunks/main.js.map": "recon",
    "/README.md": "recon",
    "/reconx-632e35b2b6e7": "recon",
}

OUR_404S = (
    "/favicon.ico", "/favicon.png", "/apple-touch-icon.png",
    "/apple-touch-icon-precomposed.png", "/ads.txt", "/app-ads.txt",
    "/robots.txt", "/sitemap.xml", "/manifest.json", "/app.js",
    "/assets/vendor/lucide.min.js.map", "/assets/blog/features/actions.webp",
    "/dashboard-composer.js", "/dashboard-plugin-host.js", "/user-status.js",
    "/forkmesh/forkmesh/commit/[id]", "/alice/site/blob/main/config.php",
    "/alice/site/raw/main/.env", "/alice/site/tree/main/wp-content",
    "/api/repo/alice/site/raw/.env", "/api/orgs/alice/repos",
    "/api/v1/timelines/public", "/api/mail/t/o/[id].gif",
    "/docs/ssh-gateway-config.schema.json/", "/.well-known/security.txt",
    "/private-or-unpublished-repository/[api:blob]", "/blog/some-post",
    "/@alice", "/alice", "/security", "/", "", "no-slash",
    "/market/previews/packages/jenkins/preview/market.png",
    "/market/previews/packages/aws-credentials/preview/market.png",
    "/market/previews/packages/sentry/preview/market.png",
    "/rails/rails/blob/main/config/initializers/stripe.rb",
    "/grafana/api/tree/main/README.md", "/api/v3/rate_limit",
    "/api/v1/instance", "/api/chat/direct-messages/[opaque]",
    "/api/market/reviews/forkmesh/diagnostics", "/blog/api-keys-explained",
    "/assets/fonts/Lato-Bold.ttf", "/index.js.map", "/mail", "/admin",
    "/alice/MY_REPO", "/alice/vendor", "/alice/kafka", "/login", "/settings",
    "/contact", "/register", "/notes", "/status.html", "/healthz",
    "/api/repo/alice/site/raw/main/MAIL_HOST",
    "/api/repo/alice/site/raw/main/backup.sql", "/alice/repo.git/info/refs",
    "/docs/swagger", "/%23pricing")


class ProbeHarness(Harness):
    def __init__(self, **env):
        super().__init__()
        self.env = SimpleNamespace(**{
            "ADMIN_PATH": "ops-console",
            "APP_ORIGIN": "https://app.forkmesh.com",
            "WWW_ORIGIN": "https://forkmesh.com",
            "WORLD_ORIGIN": "https://world.forkmesh.com", **env})

    def scan(self, path, client=None):
        return _run(self.probe.observe(
            self.env, path, CLIENT if client is None else client, self.now))

    def bans(self):
        return [dict(r) for r in self.db.execute(
            "SELECT * FROM probe_bans ORDER BY created_at, rule_id")]

    def rules(self):
        return [dict(r) for r in self.db.execute(
            "SELECT * FROM request_block_rules ORDER BY rule_id")]

    def visitor(self):
        return dict(self.db.execute("SELECT * FROM probe_visitors").fetchone())

    def denied(self, ip, path="/"):
        request = FakeRequest("https://app.forkmesh.com" + path,
                              headers={"cf-connecting-ip": ip})
        return self.probe.denied(self.env, request, path)


def test_every_scanner_family_from_the_404_list_is_classified():
    probe = ProbeHarness().probe
    assert {path: probe.classify(path) for path in SCANNER_PATHS} == (
        SCANNER_PATHS)


def test_our_own_404s_and_repository_routes_are_never_probes():
    probe = ProbeHarness().probe
    assert [path for path in OUR_404S if probe.classify(path)] == []


def test_cloudflare_ranges_match_the_caddy_renderer():
    text = (REPO / "deploy" / "core" / "render.py").read_text("utf-8")
    for node in ast.parse(text).body:
        if isinstance(node, ast.Assign) and getattr(
                node.targets[0], "id", "") == "CLOUDFLARE_RANGES":
            ranges = ast.literal_eval(node.value)
    assert ProbeHarness().probe.CLOUDFLARE_RANGES == ranges


def test_only_public_addresses_outside_cloudflare_are_ban_targets():
    h = ProbeHarness(FORKMESH_PROBE_BAN_ALLOW="93.184.216.0/24, junk")
    target = h.probe.ban_target
    plain = SimpleNamespace()
    assert target(plain, SCANNER) == SCANNER
    assert target(plain, "::ffff:" + SCANNER) == SCANNER
    assert target(plain, "2a01:4f8:1c1c:abcd::1") == "2a01:4f8:1c1c:abcd::/64"
    for address in ("127.0.0.1", "10.77.0.2", "192.168.1.5", "100.64.0.1",
                    "203.0.113.7", "::1", "fe80::1", "2001:db8::1",
                    "104.16.0.1", "172.70.1.1", "2606:4700::1", "", "x"):
        assert target(plain, address) == "", address
    assert target(h.env, SCANNER) == ""
    assert target(h.env, "93.184.217.1") == "93.184.217.1"


def test_one_secret_probe_bans_the_visitor_everywhere():
    h = ProbeHarness()
    assert h.denied(SCANNER) is None
    assert h.scan("/.env") == "banned"
    [rule] = h.rules()
    assert rule["rule_id"].startswith("auto-probe-")
    assert (rule["match_type"], rule["pattern"], rule["enabled"],
            rule["priority"]) == ("ip", SCANNER, 1, 0)
    assert SCANNER not in rule["note"] and "secret" in rule["note"]
    [ban] = h.bans()
    assert ban["rule_id"] == rule["rule_id"] and ban["enforced"] == 1
    assert ban["expires_at"] == NOW + 7 * DAY and ban["score"] == 3
    assert ban["kinds"] == "secret"
    assert [p for p, _ in json.loads(ban["paths"])] == ["/.env"]
    assert json.loads(ban["client"])["target"] == SCANNER

    response = h.denied(SCANNER, "/wp-admin")
    assert response.status == 403 and response.headers["cache-control"] == (
        "no-store")
    assert h.denied(SCANNER, "/health") is None
    assert h.denied(SCANNER, "/api/fleet/peers") is None
    assert h.denied(OTHER) is None
    assert h.scan("/.git/config") == "banned"
    assert len(h.rules()) == 1


def test_mixed_families_add_up_and_one_stray_404_does_not():
    h = ProbeHarness()
    assert h.scan("/config.json") == "scored"
    assert h.scan("/favicon.ico") == ""
    assert h.scan("/config.json") == "seen"
    assert h.scan("/Dockerfile") == "scored"
    assert h.visitor()["score"] == 2
    assert h.scan("/kafka/") == "banned"
    assert h.bans()[0]["kinds"] == "config,recon"


def test_one_script_probe_bans_and_one_backup_probe_does_not():
    h = ProbeHarness()
    assert h.scan("/backup.zip") == "scored"
    assert h.rules() == []
    assert h.scan("//xmlrpc.php", {"ip": OTHER}) == "banned"
    assert h.scan("/wp-includes/", {"ip": THIRD}) == "banned"
    assert sorted(b["kinds"] for b in h.bans()) == ["script", "script"]
    assert len(h.rules()) == 2


def test_the_score_window_is_one_day():
    h = ProbeHarness()
    h.scan("/backup.zip")
    h.now += DAY - 1
    assert h.scan("/kafka/") == "banned"

    late = ProbeHarness()
    late.scan("/backup.zip")
    late.now += DAY
    assert late.scan("/kafka/") == "scored"
    assert late.visitor()["score"] == 1
    assert late.rules() == []


def test_a_repeat_offender_is_banned_for_a_month():
    h = ProbeHarness()
    assert h.scan("/.env") == "banned"
    assert h.bans()[0]["expires_at"] == NOW + 7 * DAY
    h.now += 60 * DAY
    _run(h.probe.sweep(h.env, h.now))
    assert h.rules() == []
    assert h.scan("/.git/config") == "banned"
    assert h.bans()[0]["expires_at"] == h.now + 30 * DAY
    assert h.visitor()["bans"] == 2


def test_an_ipv6_scanner_is_banned_by_its_slash_64():
    h = ProbeHarness()
    first = {"ip": "2a01:4f8:1c1c:abcd::1"}
    h.scan("/.env", first)
    assert h.scan("/.env.local", {"ip": "2a01:4f8:1c1c:abcd::99"}) == "banned"
    [rule] = h.rules()
    assert (rule["match_type"], rule["pattern"]) == (
        "cidr", "2a01:4f8:1c1c:abcd::/64")
    assert h.denied("2a01:4f8:1c1c:abcd:ffff::7").status == 403
    assert h.denied("2a01:4f8:1c1c:abce::1") is None


def test_observe_mode_records_without_blocking_and_off_does_nothing():
    h = ProbeHarness(FORKMESH_PROBE_BAN="observe")
    assert h.scan("/.env") == "observed"
    assert h.rules() == [] and h.bans()[0]["enforced"] == 0
    assert h.denied(SCANNER) is None
    assert (h.visitor()["bans"], h.visitor()["banned_until"]) == (0, 0)
    h.env.FORKMESH_PROBE_BAN = "ban"
    assert h.scan("/.aws/credentials") == "banned"
    assert h.bans()[0]["expires_at"] == NOW + 7 * DAY and len(h.rules()) == 1

    off = ProbeHarness(FORKMESH_PROBE_BAN="off")
    assert off.scan("/.env") == "" and off.scan("/.git/config") == ""
    assert off.db.execute("SELECT COUNT(*) FROM probe_visitors").fetchone()[0] == 0


def test_cloudflare_and_private_visitors_are_never_scored():
    h = ProbeHarness()
    for ip in ("104.16.0.1", "10.77.0.2", "127.0.0.1", ""):
        assert h.scan("/.env", {"ip": ip}) == ""
        assert h.scan("/.git/config", {"ip": ip}) == ""
    assert h.db.execute("SELECT COUNT(*) FROM probe_visitors").fetchone()[0] == 0


def test_an_existing_manual_rule_for_the_address_is_left_alone():
    h = ProbeHarness()
    h.db.execute(
        "INSERT INTO request_block_rules (rule_id,match_type,pattern,enabled,"
        "priority,note,created_at,updated_at) VALUES "
        "('manual-1','ip',?,1,100,'operator',1,1)", (SCANNER,))
    h.scan("/.env")
    assert h.scan("/.git/config") == "banned"
    assert [r["rule_id"] for r in h.rules()] == ["manual-1"]
    _run(h.probe.sweep(h.env, h.now + 2 * DAY))
    assert [r["rule_id"] for r in h.rules()] == ["manual-1"]


def test_active_bans_are_capped(monkeypatch):
    h = ProbeHarness()
    monkeypatch.setattr(h.probe, "MAX_ACTIVE_BANS", 1)
    h.scan("/.env")
    assert h.scan("/.git/config") == "banned"
    h.scan("/.env", {"ip": OTHER})
    assert h.scan("/.git/config", {"ip": OTHER}) == "observed"
    assert len(h.rules()) == 1
    assert [b["enforced"] for b in h.bans()] == [1, 0]


def test_the_sweep_expires_bans_and_turning_off_lifts_every_auto_rule():
    h = ProbeHarness()
    h.db.execute(
        "INSERT INTO request_block_rules (rule_id,match_type,pattern,enabled,"
        "priority,note,created_at,updated_at) VALUES "
        "('manual-1','user_agent_keyword','badbot',1,100,'',1,1)")
    h.scan("/.env")
    h.scan("/.git/config")
    h.db.execute(
        "INSERT INTO request_block_rules (rule_id,match_type,pattern,enabled,"
        "priority,note,created_at,updated_at) VALUES "
        "('auto-probe-orphan','ip','93.184.216.99',1,0,'',1,1)")
    _run(h.probe.sweep(h.env, h.now + HOUR))
    assert [r["rule_id"] for r in h.rules()] == [
        h.bans()[0]["rule_id"], "manual-1"]

    h.env.FORKMESH_PROBE_BAN = "off"
    _run(h.probe.sweep(h.env, h.now + HOUR))
    assert [r["rule_id"] for r in h.rules()] == ["manual-1"]
    assert h.local_bans == {}

    h.env.FORKMESH_PROBE_BAN = "ban"
    _run(h.probe.sweep(h.env, h.now + 15 * DAY))
    assert h.bans() == []
    _run(h.probe.sweep(h.env, h.now + 100 * DAY))
    assert h.db.execute("SELECT COUNT(*) FROM probe_visitors").fetchone()[0] == 0


def test_the_404_log_scores_probes_even_on_ignored_paths():
    h = ProbeHarness()
    h.db.execute(
        "INSERT INTO not_found_ignores (site,path,prefix,created_at) "
        "VALUES ('forkmesh.com','/.env',0,1)")
    assert not _run(h.log.record(
        h.env, "forkmesh.com", "GET", "/.env", now=h.now, client=CLIENT))
    request = FakeRequest("https://app.forkmesh.com/.git/config",
                          headers={"cf-connecting-ip": SCANNER})
    assert _run(h.log.record_response(
        h.env, request, urlparse(request.url)))
    [ban] = h.bans()
    assert [p for p, _ in json.loads(ban["paths"])] == ["/.env"]
    assert ban["score"] == 3 and len(h.rules()) == 1


def test_admin_page_lists_bans_marks_probes_and_lifts_one():
    h = ProbeHarness()
    h.hit("/.env", client=CLIENT)
    h.hit("/favicon.ico")
    h.hit("/wp-login.php", client=CLIENT)
    h.hit("/.git/config", client=CLIENT)
    page = h.admin().body
    assert "Auto bans" in page and "On: a ban is enforced" in page
    assert SCANNER in page and "zgrab" in page and "Secrets" in page
    assert page.count('class="pill bad">Probe</span>') == 3
    assert "lift_probe_ban" in page
    rule_id = h.bans()[0]["rule_id"]

    rejected = h.admin(method="POST", form={
        "csrf": "nope", "action": "lift_probe_ban", "rule_id": rule_id})
    assert "notice=csrf" in rejected.headers["location"]
    assert len(h.rules()) == 1

    response = h.admin(method="POST", form={
        "csrf": CSRF, "action": "lift_probe_ban", "rule_id": rule_id})
    assert "notice=ban_lifted" in response.headers["location"]
    assert h.rules() == [] and h.local_bans == {}
    ban = h.bans()[0]
    assert ban["lifted_by"] == "alice" and ban["expires_at"] == h.now
    assert h.visitor()["bans"] == 0 and h.visitor()["banned_until"] == 0
    assert h.audits[-1][1] == "admin.console_lift_probe_ban"
    assert "Lifted" in h.admin().body

    for bad in ("manual-1", "auto-probe-" + "x" * 80):
        response = h.admin(method="POST", form={
            "csrf": CSRF, "action": "lift_probe_ban", "rule_id": bad})
        assert "notice=ban_invalid" in response.headers["location"]
    response = h.admin(method="POST", form={
        "csrf": CSRF, "action": "lift_probe_ban", "rule_id": "auto-probe-none"})
    assert "notice=ban_missing" in response.headers["location"]


def test_the_waf_sync_never_publishes_auto_bans():
    spec = importlib.util.spec_from_file_location(
        "sync_rules_under_test", ROOT / "tools" / "sync_request_block_rules.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    db = sqlite3.connect(":memory:")
    db.execute(
        "CREATE TABLE request_block_rules (rule_id TEXT, match_type TEXT, "
        "pattern TEXT, enabled INTEGER, priority INTEGER, created_at INTEGER)")
    db.executemany(
        "INSERT INTO request_block_rules VALUES (?,?,?,1,?,1)",
        [("manual-1", "ip", "198.51.100.1", 100),
         ("auto-probe-abc", "ip", SCANNER, 0)])
    assert [row[0] for row in db.execute(module.ENABLED_RULES_SQL)] == [
        "manual-1"]


@pytest.mark.parametrize("mode", ["ban", "observe", "off", "", "nonsense"])
def test_mode_defaults_to_ban(mode):
    expected = mode if mode in ("ban", "observe", "off") else "ban"
    assert ProbeHarness().probe.mode(
        SimpleNamespace(FORKMESH_PROBE_BAN=mode)) == expected


def test_a_page_cannot_get_its_visitors_banned_by_embedding_probe_paths():
    h = ProbeHarness()
    for site in ("cross-site", "same-site", "same-origin", "Cross-Site"):
        for path in ("/.env", "/.git/config", "/wp-login.php"):
            request = FakeRequest(
                "https://app.forkmesh.com" + path,
                headers={"cf-connecting-ip": SCANNER, "sec-fetch-site": site})
            _run(h.log.record_response(h.env, request, urlparse(request.url)))
            _run(h.log.relay_handler(h.env, FakeRequest(
                "https://app.forkmesh.com/api/not-found", "POST",
                json.dumps({"path": path, "fetchSite": site}),
                headers={"cf-connecting-ip": SCANNER})))
    assert h.db.execute("SELECT COUNT(*) FROM probe_visitors").fetchone()[0] == 0
    assert len(h.rows()) == 6

    for path in ("/.env", "/.git/config"):
        _run(h.log.relay_handler(h.env, FakeRequest(
            "https://app.forkmesh.com/api/not-found", "POST",
            json.dumps({"path": path, "fetchSite": "none"}),
            headers={"cf-connecting-ip": SCANNER})))
    assert len(h.rules()) == 1
