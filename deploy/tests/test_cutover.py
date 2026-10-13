"""The production cutover tooling (docs/CUTOVER.md)."""

import base64
import hashlib
import json
import os
import sqlite3
import subprocess
import sys
import urllib.request
from pathlib import Path

import pytest

import cutover_data
import cutover_dns
import cutover_rehost
import cutover_tls
import next_rekey
import render
from test_certsync import OPENSSL, make_ca, make_leaf

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
KEYS = {"n1": "k" * 42 + "A=", "n2": "m" * 42 + "E=", "n3": "p" * 42 + "I="}
ACCOUNT = "0123456789abcdef0123456789abcdef"
ZONE = "fedcba9876543210fedcba9876543210"
PRODUCTION = {"app_host": "app.forkmesh.com", "world_host": "world.forkmesh.com",
              "vars": {"PUBLIC_BASE_URL": "https://forkmesh.com",
                       "APP_ORIGIN": "https://app.forkmesh.com",
                       "CORS_ALLOWED_ORIGINS": "https://forkmesh.com,https://app.forkmesh.com"}}


def fleet(tmp_path, production=None):
    state = tmp_path / "state.json"
    state.write_text(json.dumps({"wg_pubkeys": KEYS}))
    loaded = render.load_fleet(FLEET, state_path=state)
    if production is not None:
        loaded["production"] = production
        render.check_production(loaded)
    return loaded


def rendered(loaded, node="n1"):
    files, manifest = render.render_core(loaded, node)
    return {path: content for path, (content, _) in files.items()}, manifest


def test_without_production_nothing_changes(tmp_path):
    files, manifest = rendered(fleet(tmp_path))
    assert "FORKMESH_SERVES_PRODUCTION" not in files["/etc/forkmesh-next/app.env"]
    assert "app.forkmesh.com" not in files["/etc/caddy/Caddyfile"]
    assert manifest["tls_hosts"] == manifest["shared_hosts"]


def test_production_identity_renders_everywhere(tmp_path):
    loaded = fleet(tmp_path, PRODUCTION)
    files, manifest = rendered(loaded)
    caddy = files["/etc/caddy/Caddyfile"]
    env = files["/etc/forkmesh-next/app.env"]
    assert "app.forkmesh.com {" in caddy and "world.forkmesh.com {" in caddy
    app_next = caddy.split("app.next.forkmesh.com {", 1)[1].split("\n}\n", 1)[0]
    assert "redir https://app.forkmesh.com{uri} 308" in app_next
    assert "@readiness path /readyz /health /api/version" in app_next
    world_next = caddy.split("world.next.forkmesh.com {", 1)[1].split("\n}\n", 1)[0]
    assert "redir https://world.forkmesh.com{uri} 308" in world_next
    assert "header_up Host app.forkmesh.com" in caddy
    certsync = json.loads(files["/etc/forkmesh-next/certsync.json"])
    assert certsync["names"] == ["app.forkmesh.com", "world.forkmesh.com",
                                 "app.next.forkmesh.com", "world.next.forkmesh.com"]
    assert "FORKMESH_SERVES_PRODUCTION=1" in env
    assert "FORKMESH_VAR_PUBLIC_BASE_URL=https://forkmesh.com" in env
    hosts_line = [l for l in env.splitlines() if l.startswith("FORKMESH_PUBLIC_HOSTS=")][0]
    assert hosts_line.startswith("FORKMESH_PUBLIC_HOSTS=app.forkmesh.com,world.forkmesh.com,")
    assert "app.next.forkmesh.com" in hosts_line
    fragment = files["/etc/forkmesh-next/hosts.fragment"]
    assert "127.0.0.1 app.forkmesh.com app.next.forkmesh.com" in fragment
    overrides = json.loads(files[render.DAEMON_OVERRIDES_PATH])
    assert overrides["catalogUrl"] == "https://app.forkmesh.com/api/repositories"
    assert "FORKMESH_DNS_NAMES=app.next.forkmesh.com,world.next.forkmesh.com" in env


@pytest.mark.parametrize("change,match", [
    ({"app_host": "app.forkmesh.co"}, "app_host"),
    ({"world_host": "evil.example"}, "world_host"),
    ({"vars": {"DATA_KEY": "x"}}, "secret"),
    ({"vars": {"MAILTRAP_API_TOKEN": "x"}}, "secret"),
    ({"vars": {"NOT_A_WRANGLER_VAR": "x"}}, "not an app/wrangler.toml"),
    ({"vars": {"PUBLIC_BASE_URL": "a\nFORKMESH_VAULT=0"}}, "one-line"),
    ({"extra": 1}, "unknown"),
])
def test_production_table_is_validated(tmp_path, change, match):
    bad = dict(PRODUCTION, **change)
    with pytest.raises(render.RenderError, match=match):
        fleet(tmp_path, bad)


def test_egress_switch(monkeypatch):
    sys.path.insert(0, str(REPO / "app" / "host"))
    from forkmesh_host import egress
    assert egress.production_host("mirror35.forkmesh.com", environ={})
    assert not egress.production_host("mirror35.forkmesh.com",
                                      environ={"FORKMESH_SERVES_PRODUCTION": "1"})
    with pytest.raises(egress.EgressBlocked):
        monkeypatch.delenv("FORKMESH_SERVES_PRODUCTION", raising=False)
        egress.check_url_host("app.forkmesh.com", 443, "https")
    monkeypatch.setenv("FORKMESH_SERVES_PRODUCTION", "1")
    egress.check_url_host("app.forkmesh.com", 443, "https")
    assert egress.address_blocked("10.77.0.1")


class FakeCloudflare:

    def __init__(self):
        self.domains = [
            {"id": "d-app", "hostname": "app.forkmesh.com", "service": "forkmesh-app",
             "environment": "production"},
            {"id": "d-world", "hostname": "world.forkmesh.com",
             "service": "forkmesh-world", "environment": "production"}]
        self.records = {
            "r-app": {"id": "r-app", "type": "AAAA", "name": "app.forkmesh.com",
                      "content": "100::", "proxied": True, "meta": {"read_only": True}},
            "r-world": {"id": "r-world", "type": "AAAA", "name": "world.forkmesh.com",
                        "content": "100::", "proxied": True, "meta": {"read_only": True}},
            "r-other": {"id": "r-other", "type": "A", "name": "forkmesh.com",
                        "content": "1.2.3.4"}}
        self.calls = []
        self.soa = {"min_ttl": 1800, "refresh": 10000}
        self.next_id = 0

    def __call__(self, request, timeout=None):
        from urllib.parse import parse_qs, urlsplit
        url = urlsplit(request.full_url)
        method, path = request.get_method(), url.path.replace("/client/v4", "")
        query = {k: v[0] for k, v in parse_qs(url.query).items()}
        body = json.loads(request.data) if request.data else None
        self.calls.append((method, path))
        result = self.handle(method, path, query, body)
        return _Response({"success": True, "result": result})

    def handle(self, method, path, query, body):
        if path == "/accounts/%s/workers/domains" % ACCOUNT:
            if method == "GET":
                return [d for d in self.domains if d["hostname"] == query["hostname"]]
            if method == "PUT":
                self.domains.append(dict(body, id="d-new-" + body["hostname"]))
                self.records["m-" + body["hostname"]] = {
                    "id": "m-" + body["hostname"], "type": "AAAA",
                    "name": body["hostname"], "content": "100::",
                    "meta": {"read_only": True}}
                return self.domains[-1]
        if path.startswith("/accounts/%s/workers/domains/" % ACCOUNT) and method == "DELETE":
            domain_id = path.rsplit("/", 1)[1]
            gone = [d for d in self.domains if d["id"] == domain_id][0]
            self.domains.remove(gone)
            for key in [k for k, r in self.records.items()
                        if r["name"] == gone["hostname"] and r["meta"].get("read_only")]:
                del self.records[key]
            return {}
        if path == "/zones/%s/dns_records" % ZONE:
            if method == "GET":
                return [r for r in self.records.values() if r["name"] == query["name"]
                        and ("type" not in query or r["type"] == query["type"])]
            self.next_id += 1
            record = dict(body, id="new-%d" % self.next_id, meta={})
            self.records[record["id"]] = record
            return record
        if path.startswith("/zones/%s/dns_records/" % ZONE):
            record_id = path.rsplit("/", 1)[1]
            if method == "GET":
                return self.records[record_id]
            if method == "DELETE":
                return self.records.pop(record_id)
        if path == "/zones/%s/dns_settings" % ZONE:
            if method == "PATCH":
                self.soa = body["soa"]
            return {"soa": self.soa}
        raise AssertionError("unexpected %s %s" % (method, path))


class _Response:
    def __init__(self, payload):
        self.payload = payload

    def read(self):
        return json.dumps(self.payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def _cf(fake):
    return cutover_dns.Cloudflare("token", ACCOUNT, ZONE, opener=fake)


def test_dns_guard():
    for bad in ("forkmesh.com", "www.forkmesh.com", "app.next.forkmesh.com",
                "mirror35.forkmesh.com", "app.forkmesh.com.evil"):
        with pytest.raises(cutover_dns.GuardError):
            cutover_dns.guard_host(bad)
    with pytest.raises(cutover_dns.GuardError):
        cutover_dns.guard_ip("1.2.3.4", ["203.0.113.11"])


def test_flip_dry_run_changes_nothing(tmp_path):
    fake = FakeCloudflare()
    loaded = fleet(tmp_path)
    assert cutover_dns.flip(_cf(fake), loaded, execute=False,
                            state_file=tmp_path / "s.json") is None
    assert all(method == "GET" for method, _ in fake.calls)


def test_flip_and_rollback(tmp_path):
    fake = FakeCloudflare()
    loaded = fleet(tmp_path)
    ips = cutover_dns.core_ips(loaded)
    state_file = tmp_path / "state.json"
    cutover_dns.flip(_cf(fake), loaded, execute=True, state_file=state_file)
    for host in cutover_dns.FLIP:
        assert not [d for d in fake.domains if d["hostname"] == host]
        a = sorted(r["content"] for r in fake.records.values()
                   if r["name"] == host and r["type"] == "A")
        assert a == sorted(ips)
        assert all(r["ttl"] == 60 and r["proxied"] is False
                   for r in fake.records.values() if r["name"] == host)
    assert fake.records["r-other"]["content"] == "1.2.3.4"
    methods = [m for m, p in fake.calls if m != "GET"]
    assert methods[:1 + len(ips)] == ["DELETE"] + ["POST"] * len(ips)
    before = len(fake.records)
    cutover_dns.flip(_cf(fake), loaded, execute=True, state_file=state_file)
    assert len(fake.records) == before
    cutover_dns.rollback(_cf(fake), loaded, execute=True, state_file=state_file)
    for host, service in cutover_dns.FLIP.items():
        assert [d["service"] for d in fake.domains if d["hostname"] == host] == [service]
        assert not [r for r in fake.records.values()
                    if r["name"] == host and r["type"] == "A"]
    assert fake.records["r-other"]["content"] == "1.2.3.4"


def test_flip_refuses_unexpected_records(tmp_path):
    fake = FakeCloudflare()
    fake.records["x"] = {"id": "x", "type": "A", "name": "app.forkmesh.com",
                         "content": "9.9.9.9", "meta": {}}
    with pytest.raises(cutover_dns.GuardError):
        cutover_dns.flip(_cf(fake), fleet(tmp_path), execute=True,
                         state_file=tmp_path / "s.json")


def test_rollback_never_deletes_a_foreign_record(tmp_path):
    fake = FakeCloudflare()
    loaded = fleet(tmp_path)
    state_file = tmp_path / "state.json"
    state_file.write_text(json.dumps({"flipped": {"app.forkmesh.com": {
        "records": ["r-other"], "domain": {"service": "forkmesh-app"}}}}))
    with pytest.raises(cutover_dns.GuardError):
        cutover_dns.rollback(_cf(fake), loaded, execute=True, state_file=state_file)
    assert "r-other" in fake.records


def test_soa_min_ttl(tmp_path):
    fake = FakeCloudflare()
    _cf(fake).set_soa_min_ttl(60)
    assert fake.soa == {"min_ttl": 60, "refresh": 10000}


def test_tls_txt_guard():
    cutover_tls.guard_txt_name("_acme-challenge.app.forkmesh.com",
                               cutover_tls.PRODUCTION_NAMES)
    for bad in ("app.forkmesh.com", "_acme-challenge.forkmesh.com",
                "_acme-challenge.www.forkmesh.com", "x._acme-challenge.app.forkmesh.com"):
        with pytest.raises(cutover_tls.GuardError):
            cutover_tls.guard_txt_name(bad, cutover_tls.PRODUCTION_NAMES)


def test_cloudflare_txt_creates_and_deletes_only_its_own_records():
    calls = []
    records = {}

    def opener(request, timeout=None):
        method, path = request.get_method(), request.full_url.split("/client/v4")[1]
        calls.append((method, path))
        if method == "POST":
            body = json.loads(request.data)
            records["t1"] = dict(body, id="t1")
            return _Response({"success": True, "result": records["t1"]})
        if method == "GET":
            return _Response({"success": True, "result": records["t1"]})
        return _Response({"success": True, "result": {}})

    txt = cutover_tls.CloudflareTxt("token", ZONE, opener=opener)
    txt.set_txt("app.forkmesh.com", "value")
    assert records["t1"]["type"] == "TXT"
    assert records["t1"]["name"] == "_acme-challenge.app.forkmesh.com"
    with pytest.raises(cutover_tls.GuardError):
        txt.set_txt("forkmesh.com", "value")
    txt.clear("app.forkmesh.com")
    assert calls[-1] == ("DELETE", "/zones/%s/dns_records/t1" % ZONE)
    calls.clear()
    txt.clear("world.forkmesh.com")
    assert calls == []


def test_acme_dns01_value_matches_rfc8555():
    acme = cutover_tls.Acme.__new__(cutover_tls.Acme)
    acme.thumbprint = "thumb"
    expected = base64.urlsafe_b64encode(hashlib.sha256(b"tok.thumb").digest()
                                        ).rstrip(b"=").decode()
    assert acme.dns_value("tok") == expected


def test_issue_dry_run_touches_nothing(tmp_path):
    fake = cutover_tls.CloudflareTxt("dry-run", ZONE, opener=lambda *a, **k: 1 / 0)
    assert cutover_tls.issue(list(cutover_tls.PRODUCTION_NAMES), fake,
                             cutover_tls.LE_DIRECTORY, execute=False,
                             out_dir=tmp_path) == {}


@pytest.mark.skipif(OPENSSL is None, reason="openssl not installed")
def test_pushed_pair_is_served_by_certsync_before_the_flip(tmp_path):
    import certsync
    ca = make_ca(tmp_path)
    loaded = fleet(tmp_path, PRODUCTION)
    files, _ = rendered(loaded, "n2")
    config = json.loads(files["/etc/forkmesh-next/certsync.json"])
    root = tmp_path / "n2"
    config.update({"tls_dir": str(root / "tls"), "include_dir": str(root / "caddy"),
                   "export_dir": "", "caddy_storage": str(root / "storage"),
                   "ca_file": str(ca[0])})
    (root / "tls").mkdir(parents=True)
    for name in cutover_tls.PRODUCTION_NAMES:
        cert, key = make_leaf(tmp_path, name, 80, ca, intermediate=True)
        cutover_tls.validate(name, cert, key, ca_file=ca[0])
        (root / "tls" / (name + ".crt")).write_bytes(cert)
        (root / "tls" / (name + ".key")).write_bytes(key)
    report = certsync.sync(config, b"", reload=["false"], owner="", offline=True)
    for name in cutover_tls.PRODUCTION_NAMES:
        assert report["modes"][name] == "files"
        include = (root / "caddy" / ("tls-%s.caddy" % name)).read_text()
        assert "%s/%s.crt" % (config["tls_dir"], name) in include


@pytest.mark.parametrize("before,after", [
    ('"https://next.forkmesh.com/docs"', '"https://forkmesh.com/docs"'),
    ("app.next.forkmesh.com", "app.forkmesh.com"),
    ("wss://world.next.forkmesh.com/ws", "wss://world.forkmesh.com/ws"),
    ("git@ssh.next.forkmesh.com:o/r", "git@ssh.forkmesh.com:o/r"),
    ('"; Domain=.next.forkmesh.com"', '"; Domain=.forkmesh.com"'),
    ("https://n1.next.forkmesh.com/_app", "https://n1.next.forkmesh.com/_app"),
    ('["next.forkmesh.com", "next.forkmesh.com"]', '["forkmesh.com", "www.forkmesh.com"]'),
    ("founders@forkmesh.com", "founders@forkmesh.com"),
    ("notnext.forkmesh.com", "notnext.forkmesh.com"),
])
def test_inverse_rehost(before, after):
    assert cutover_rehost.rewrite_text(before)[0] == after


def test_rehost_roundtrip_of_mapped_hosts():
    import rehost
    for prod in ("https://forkmesh.com/x", "app.forkmesh.com", "world.forkmesh.com",
                 "ssh.forkmesh.com", ".forkmesh.com"):
        assert cutover_rehost.rewrite_text(rehost.rewrite_text(prod)[0])[0] == prod


def test_inverse_rehost_refuses_a_main_checkout(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)], check=True)
    assert cutover_rehost.main(["--apply", "--root", str(repo)]) == 2
    subprocess.run(["git", "-C", str(repo), "checkout", "-q", "-b", "cutover/x"],
                   check=True)
    (repo / "app").mkdir()
    (repo / "app" / "wrangler.toml").write_text('X = "https://next.forkmesh.com"\n')
    assert cutover_rehost.main(["--apply", "--root", str(repo)]) == 0
    assert (repo / "app" / "wrangler.toml").read_text() == 'X = "https://forkmesh.com"\n'


def test_old_key_from_env_file(tmp_path):
    env = tmp_path / ".env.production"
    env.write_text("# comment\nCLOUDFLARE_API_TOKEN=x\nDATA_KEY='%s'\n" % ("k" * 40))
    env.chmod(0o600)
    keyset = next_rekey.read_key_from_env_file("%s:DATA_KEY" % env, "old")
    assert keyset.same_as(next_rekey.Keyset("k" * 40, "x"))
    env.chmod(0o644)
    with pytest.raises(next_rekey.RekeyError, match="group/other"):
        next_rekey.read_key_from_env_file("%s:DATA_KEY" % env, "old")
    env.chmod(0o600)
    with pytest.raises(next_rekey.RekeyError, match="exactly one"):
        next_rekey.read_key_from_env_file("%s:MISSING" % env, "old")
    with pytest.raises(next_rekey.RekeyError, match="FILE:NAME"):
        next_rekey.read_key_from_env_file(str(env), "old")


def _strict_db(path, old, dead_seal=None):
    conn = sqlite3.connect(path)
    conn.execute("CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,"
                 " email_bi TEXT, username TEXT)")
    conn.execute("CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, owner_bi TEXT"
                 " NOT NULL, data TEXT NOT NULL)")
    rec = {"name": "alice", "email": "alice@example.com", "status": "active"}
    conn.execute("INSERT INTO users VALUES (?,?,?,?)", (
        old.bi("alice"), old.seal(json.dumps(rec).encode()),
        old.bi("alice@example.com"), "alice"))
    conn.execute("INSERT INTO repositories VALUES (?,?,?)", (
        old.bi("alice/tool"), old.bi("alice"),
        old.seal(json.dumps({"owner": "alice", "repo": "tool"}).encode())))
    sealer = dead_seal or next_rekey.Keyset("z" * 40, "lost")
    conn.execute("INSERT INTO repositories VALUES (?,?,?)", (
        hashlib.sha256(b"unknown").hexdigest(), old.bi("alice"),
        sealer.seal(json.dumps({"owner": "ghost", "repo": "gone"}).encode())))
    conn.commit()
    conn.close()


def test_dead_rows_do_not_block_the_rekey(tmp_path):
    old, new = next_rekey.Keyset("o" * 40, "old"), next_rekey.Keyset("n" * 40, "new")
    source = tmp_path / "in.sqlite3"
    _strict_db(source, old)
    out = tmp_path / "out.sqlite3"
    summary = next_rekey.rekey(source, out, old, new, tmp_path / "work",
                               out=open(os.devnull, "w"))
    assert summary["blind_indexes"]["dead_row_strict_pseudonymised"] == 1
    conn = sqlite3.connect(out)
    keys = {r[0] for r in conn.execute("SELECT key_bi FROM repositories")}
    assert new.bi("alice/tool") in keys and len(keys) == 2


def test_a_live_orphan_still_aborts(tmp_path):
    old, new = next_rekey.Keyset("o" * 40, "old"), next_rekey.Keyset("n" * 40, "new")
    source = tmp_path / "in.sqlite3"
    _strict_db(source, old, dead_seal=old)
    conn = sqlite3.connect(source)
    conn.execute("UPDATE repositories SET data=? WHERE key_bi=?", (
        old.seal(json.dumps({"note": "no names here"}).encode()),
        hashlib.sha256(b"unknown").hexdigest()))
    conn.commit()
    conn.close()
    with pytest.raises(next_rekey.RekeyError, match="repositories.key_bi"):
        next_rekey.rekey(source, tmp_path / "out.sqlite3", old, new,
                         tmp_path / "work", out=open(os.devnull, "w"))


def _tables(conn, with_next=True):
    conn.executescript("""
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,
            email_bi TEXT, username TEXT, is_admin INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE account_devices (device_bi TEXT PRIMARY KEY, account_bi TEXT);
        CREATE TABLE nodes (node_bi TEXT PRIMARY KEY, user_bi TEXT, pubkey TEXT,
            data TEXT NOT NULL, name TEXT, last_seen INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE mirror_https_endpoints (node_bi TEXT PRIMARY KEY,
            node_name TEXT NOT NULL, base_url TEXT NOT NULL);
        CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, owner_bi TEXT NOT NULL,
            data TEXT NOT NULL);
    """)
    if with_next:
        conn.executescript("""
            CREATE TABLE host_secrets (name TEXT PRIMARY KEY, ciphertext TEXT);
            CREATE TABLE cluster_members (node TEXT PRIMARY KEY, status TEXT);
            CREATE TABLE hosted_repos (id TEXT PRIMARY KEY, key_bi TEXT NOT NULL,
                deleted_at INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE runner_request_nonces (nonce TEXT PRIMARY KEY);
        """)


def _next_and_final(tmp_path, key):
    source, target = tmp_path / "next.sqlite3", tmp_path / "final.sqlite3"
    src = sqlite3.connect(source)
    _tables(src)
    for name in ("fmnext", "alice"):
        src.execute("INSERT INTO users VALUES (?,?,?,?,0)", (
            key.bi(name), key.seal(json.dumps({"name": name}).encode()), None, name))
    src.execute("INSERT INTO account_devices VALUES ('dev1', ?)", (key.bi("fmnext"),))
    for name in ("fmnext-n1", "mirror35"):
        src.execute("INSERT INTO nodes (node_bi, data, name) VALUES (?,?,?)",
                    (key.bi(name), "x", name))
        src.execute("INSERT INTO mirror_https_endpoints VALUES (?,?,?)",
                    (key.bi(name), name, "https://" + name))
    src.execute("INSERT INTO host_secrets VALUES ('ADMIN_PASS', 'sealed')")
    src.execute("INSERT INTO cluster_members VALUES ('n1', 'active')")
    src.execute("INSERT INTO hosted_repos VALUES ('h1', ?, 0)", (key.bi("fmnext/site"),))
    src.execute("INSERT INTO repositories VALUES (?,?,?)",
                (key.bi("fmnext/site"), key.bi("fmnext"), "x"))
    src.execute("INSERT INTO runner_request_nonces VALUES ('n')")
    src.commit()
    src.close()
    dst = sqlite3.connect(target)
    _tables(dst)
    dst.execute("INSERT INTO users VALUES (?,?,?,?,0)", (
        key.bi("alice"), key.seal(json.dumps({"name": "alice"}).encode()), None, "alice"))
    dst.execute("INSERT INTO nodes (node_bi, data, name) VALUES (?,?,?)",
                (key.bi("mirror35"), "prod", "mirror35"))
    dst.commit()
    dst.close()
    return source, target


def test_carry_copies_only_next_owned_rows(tmp_path):
    key = next_rekey.Keyset("n" * 40, "next")
    source, target = _next_and_final(tmp_path, key)
    cutover_data.carry(source, target, ["fmnext"], execute=False)
    conn = sqlite3.connect(target)
    assert conn.execute("SELECT COUNT(*) FROM users").fetchone()[0] == 1
    conn.close()
    cutover_data.carry(source, target, ["fmnext"], execute=True)
    conn = sqlite3.connect(target)
    assert sorted(r[0] for r in conn.execute("SELECT username FROM users")) == [
        "alice", "fmnext"]
    assert sorted(r[0] for r in conn.execute("SELECT name FROM nodes")) == [
        "fmnext-n1", "mirror35"]
    assert conn.execute("SELECT data FROM nodes WHERE name='mirror35'").fetchone()[0] == "prod"
    assert conn.execute("SELECT COUNT(*) FROM account_devices").fetchone()[0] == 1
    assert conn.execute("SELECT COUNT(*) FROM host_secrets").fetchone()[0] == 1
    assert conn.execute("SELECT COUNT(*) FROM hosted_repos").fetchone()[0] == 1
    assert conn.execute("SELECT COUNT(*) FROM repositories").fetchone()[0] == 1
    assert conn.execute("SELECT COUNT(*) FROM runner_request_nonces").fetchone()[0] == 0
    conn.close()


def test_carry_refuses_a_clash(tmp_path):
    key = next_rekey.Keyset("n" * 40, "next")
    source, target = _next_and_final(tmp_path, key)
    with pytest.raises(cutover_data.CutoverError, match="already exists"):
        cutover_data.carry(source, target, ["alice"], execute=True)
    conn = sqlite3.connect(target)
    assert conn.execute("SELECT COUNT(*) FROM host_secrets").fetchone()[0] == 0
    conn.close()


def test_verify_counts_and_baseline(tmp_path):
    key = next_rekey.Keyset("n" * 40, "next")
    old = next_rekey.Keyset("o" * 40, "old")
    for path, k in ((tmp_path / "base.sqlite3", old), (tmp_path / "final.sqlite3", key)):
        conn = sqlite3.connect(path)
        _tables(conn, with_next=False)
        good = {"name": "alice", "email": "a@example.com"}
        stale = {"name": "bob", "email": "b@example.com"}
        conn.execute("INSERT INTO users VALUES (?,?,?,?,0)", (
            k.bi("alice"), k.seal(json.dumps(good).encode()), k.bi("a@example.com"), "alice"))
        conn.execute("INSERT INTO users VALUES (?,?,?,?,0)", (
            k.bi("bob"), k.seal(json.dumps(stale).encode()), k.bi("old@example.com"), "bob"))
        conn.commit()
        conn.close()
    keyfile = tmp_path / "key"
    keyfile.write_text("n" * 40)
    keyfile.chmod(0o600)
    counts, problems = cutover_data.verify(tmp_path / "final.sqlite3", keyfile)
    assert counts["users_by_name"] == 2 and counts["users_by_email"] == 1
    assert problems
    env = tmp_path / "prod.env"
    env.write_text("DATA_KEY=%s\n" % ("o" * 40))
    env.chmod(0o600)
    counts, problems = cutover_data.verify(
        tmp_path / "final.sqlite3", keyfile, tmp_path / "base.sqlite3",
        "%s:DATA_KEY" % env)
    assert problems == []


def test_export_swallows_wrangler_output_and_keeps_the_token_out_of_argv(tmp_path):
    env = tmp_path / ".env.production"
    env.write_text("CLOUDFLARE_API_TOKEN=tok\nCLOUDFLARE_ACCOUNT_ID=%s\n" % ACCOUNT)
    env.chmod(0o600)
    seen = {}

    def fake_run(command, env=None, stdout=None, stderr=None, **kwargs):
        seen.update(command=command, env=env, stdout=stdout, stderr=stderr)
        Path(command[command.index("--output") + 1]).write_text("-- dump\n")
        return subprocess.CompletedProcess(command, 0)

    out = tmp_path / "dump.sql"
    assert cutover_data.export(out, execute=False, env_file=env, run=fake_run) is None
    assert not seen
    cutover_data.export(out, execute=True, env_file=env, run=fake_run)
    assert "tok" not in " ".join(seen["command"])
    assert seen["env"]["CLOUDFLARE_API_TOKEN"] == "tok"
    assert seen["stdout"] is subprocess.DEVNULL and seen["stderr"] is subprocess.DEVNULL
    assert (out.stat().st_mode & 0o777) == 0o600
    env.chmod(0o644)
    with pytest.raises(cutover_data.CutoverError, match="group/other"):
        cutover_data.export(tmp_path / "other.sql", execute=True, env_file=env,
                            run=fake_run)


def test_read_accounts(tmp_path):
    path = tmp_path / "admin.env"
    path.write_text("NEXT_ADMIN_USER=fmnext\nNEXT_ADMIN_EMAIL=a@b.c\n"
                    "NEXT_ADMIN_PASSWORD=pw\nPROBE_1_USER=x\n")
    assert cutover_data.read_accounts(path) == [("fmnext", "a@b.c", "pw")]


def _identity_files(tmp_path):
    import next_common
    config = next_common.config_dir()
    prod = tmp_path / "prod.env"
    prod.write_text("DATA_KEY=prod-data-key-0123456789abcdef0123456789\nADMIN_PASS=prod-admin\n"
                    "MIRROR_ROUTER_SIGNING_SEED=prod-seed\nMIRROR_ROUTER_PUBLIC_KEY=prod-pub\n"
                    "DISCORD_BOT_TOKEN=bot\nSENTRY_DSN=same-dsn\n"
                    "CLOUDFLARE_API_TOKEN=cf\nCLOUDFLARE_ACCOUNT_ID=%s\n" % ACCOUNT)
    prod.chmod(0o600)
    secrets_env = config / "secrets.env"
    secrets_env.write_text("# Next\nDATA_KEY=next-data-key\nADMIN_PASS=next-admin\n"
                           "MIRROR_ROUTER_SIGNING_SEED=next-seed\n"
                           "MIRROR_ROUTER_PUBLIC_KEY=next-pub\nSENTRY_DSN=same-dsn\n"
                           "WWW_EDGE_SECRET=old-edge\n")
    secrets_env.chmod(0o600)
    return prod, secrets_env


def test_identity_apply_and_rollback(tmp_path):
    import cutover_identity
    import next_common
    prod, secrets_env = _identity_files(tmp_path)
    before = secrets_env.read_text()
    assert cutover_identity.apply(False, prod) is None
    assert secrets_env.read_text() == before
    written = cutover_identity.apply(True, prod, token=lambda n: "new-edge")
    values = next_common.read_env_file(secrets_env)
    assert values["MIRROR_ROUTER_SIGNING_SEED"] == "prod-seed"
    assert values["MIRROR_ROUTER_PUBLIC_KEY"] == "prod-pub"
    assert values["DISCORD_BOT_TOKEN"] == "bot"
    assert values["WWW_EDGE_SECRET"] == "new-edge"
    assert values["DATA_KEY"] == "next-data-key" and values["ADMIN_PASS"] == "next-admin"
    assert "CLOUDFLARE_API_TOKEN" not in values
    assert "DATA_KEY" not in written and "ADMIN_PASS" not in written
    assert (secrets_env.stat().st_mode & 0o777) == 0o600
    assert secrets_env.read_text().startswith("# Next\n")
    with pytest.raises(cutover_identity.IdentityError, match="exists"):
        cutover_identity.apply(True, prod)
    cutover_identity.rollback(True)
    assert secrets_env.read_text() == before


def test_identity_put_www_secret_uses_stdin(tmp_path):
    import cutover_identity
    prod, _ = _identity_files(tmp_path)
    seen = {}

    def fake_run(command, input=None, env=None, **kwargs):
        seen.update(command=command, input=input, env=env)
        return subprocess.CompletedProcess(command, 0)

    cutover_identity.put_www_secret(True, prod, run=fake_run, wrangler=["wrangler"])
    assert seen["input"] == b"old-edge"
    assert "old-edge" not in " ".join(seen["command"])
    assert seen["command"][-2:] == ["--name", "forkmesh-www"]
    assert seen["env"]["CLOUDFLARE_API_TOKEN"] == "cf"


from test_caddy import CADDY, needs_caddy, rewrite_imports, write_certsync_includes  # noqa: E402


@needs_caddy
@pytest.mark.parametrize("modes", ["files", "acme", "internal"])
def test_production_caddyfile_adapts(tmp_path, modes):
    loaded = fleet(tmp_path, PRODUCTION)
    files, _ = render.render_core(loaded, "n2")
    render.write_tree(files, {}, tmp_path)
    write_certsync_includes(tmp_path, "n2", modes)
    config = tmp_path / "Caddyfile.test"
    config.write_text(rewrite_imports(
        (tmp_path / "etc/caddy/Caddyfile").read_text(), tmp_path))
    completed = subprocess.run(
        [CADDY, "adapt", "--config", str(config), "--adapter", "caddyfile"],
        capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    text = completed.stdout
    for host in ("app.forkmesh.com", "world.forkmesh.com",
                 "app.next.forkmesh.com", "world.next.forkmesh.com"):
        assert '"%s"' % host in text
    assert "https://app.forkmesh.com{http.request.uri}" in text
    assert "https://world.forkmesh.com{http.request.uri}" in text


def test_extract_reseal_carry_under_the_production_key(tmp_path):
    next_key = next_rekey.Keyset("n" * 40, "next")
    prod_key = next_rekey.Keyset("p" * 40, "prod")
    source, target = _next_and_final(tmp_path, next_key)
    conn = sqlite3.connect(target)
    conn.execute("DELETE FROM users")
    conn.execute("DELETE FROM nodes")
    conn.execute("INSERT INTO users VALUES (?,?,?,?,0)", (
        prod_key.bi("alice"), prod_key.seal(json.dumps({"name": "alice"}).encode()),
        None, "alice"))
    conn.commit()
    conn.close()
    conn = sqlite3.connect(source)
    conn.execute("UPDATE repositories SET data=?", (next_key.seal(json.dumps(
        {"owner": "fmnext", "repo": "site"}).encode()),))
    conn.execute("UPDATE nodes SET data=? WHERE name='fmnext-n1'", (next_key.seal(
        json.dumps({"name": "fmnext-n1"}).encode()),))
    conn.commit()
    conn.close()
    small = tmp_path / "small.sqlite3"
    cutover_data.extract(source, small, ["fmnext"])
    keyfile = tmp_path / "next.key"
    keyfile.write_text("n" * 40)
    keyfile.chmod(0o400)
    env = tmp_path / "prod.env"
    env.write_text("DATA_KEY=%s\n" % ("p" * 40))
    env.chmod(0o600)
    resealed = tmp_path / "resealed.sqlite3"
    cutover_data.reseal(small, resealed, tmp_path / "work", keyfile, "%s:DATA_KEY" % env,
                        open(os.devnull, "w"))
    cutover_data.carry(resealed, target, ["fmnext"], execute=True)
    conn = sqlite3.connect(target)
    rows = dict(conn.execute("SELECT username, data FROM users"))
    assert json.loads(prod_key.open(rows["fmnext"]))["name"] == "fmnext"
    assert next_key.open(rows["fmnext"]) is None
    names = {r[0] for r in conn.execute("SELECT user_bi FROM users")}
    assert prod_key.bi("fmnext") in names
    conn.close()
    assert cutover_data.carried_rows_open(target, resealed, prod_key, keyfile) == []
    assert cutover_data.carried_rows_open(target, source, prod_key, keyfile)


def test_identity_keep_production_key(tmp_path):
    import cutover_identity
    import next_common
    prod, secrets_env = _identity_files(tmp_path)
    secrets_env.write_text(secrets_env.read_text().replace("DATA_KEY=next-data-key\n", ""))
    config = next_common.config_dir()
    (config / "DATA_KEY").write_text("next-data-key-0123456789abcdef0123456789\n")
    (config / "DATA_KEY").chmod(0o400)
    cutover_identity.apply(True, prod, token=lambda n: "edge", keep_key=True)
    values = next_common.read_env_file(secrets_env)
    assert values["ADMIN_PASS"] == "prod-admin"
    assert "DATA_KEY" not in values
    cutover_identity.install_data_key(False, prod)
    assert (config / "DATA_KEY").read_text().strip() == "next-data-key-0123456789abcdef0123456789"
    cutover_identity.install_data_key(True, prod)
    assert (config / "DATA_KEY").read_text().strip() == "prod-data-key-0123456789abcdef0123456789"
    saved = config / cutover_identity.NEXT_KEY_NAME
    assert saved.read_text().strip() == "next-data-key-0123456789abcdef0123456789"
    assert (saved.stat().st_mode & 0o777) == 0o400
    assert ((config / "DATA_KEY").stat().st_mode & 0o777) == 0o400
    cutover_identity.install_data_key(True, prod)
    cutover_identity.restore_data_key(True)
    assert (config / "DATA_KEY").read_text().strip() == "next-data-key-0123456789abcdef0123456789"
    assert saved.exists()
