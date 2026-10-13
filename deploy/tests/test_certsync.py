"""deploy/core/certsync.py: HMAC auth, serve/pull round trip, export."""

import json
import shutil
import subprocess
import threading
import time
from pathlib import Path

import pytest

import certsync

SECRET = b"s" * 48
NAMES = ["app.next.forkmesh.com", "world.next.forkmesh.com"]
OPENSSL = shutil.which("openssl")


def test_request_auth_accepts_valid_and_rejects_everything_else():
    nonces = certsync.NonceCache()
    headers, nonce = certsync.auth_headers(SECRET, "/v1/tls/app.next.forkmesh.com", "n2",
                                           now=1000)
    assert certsync.verify_request(SECRET, "/v1/tls/app.next.forkmesh.com", headers,
                                   nonces, now=1010) == nonce
    with pytest.raises(certsync.CertSyncError, match="replayed"):
        certsync.verify_request(SECRET, "/v1/tls/app.next.forkmesh.com", headers,
                                nonces, now=1010)
    fresh, _ = certsync.auth_headers(SECRET, "/v1/tls/app.next.forkmesh.com", "n2", now=1000)
    with pytest.raises(certsync.CertSyncError, match="stale"):
        certsync.verify_request(SECRET, "/v1/tls/app.next.forkmesh.com", fresh,
                                certsync.NonceCache(), now=1000 + 61)
    with pytest.raises(certsync.CertSyncError, match="bad mac"):
        certsync.verify_request(b"x" * 48, "/v1/tls/app.next.forkmesh.com", fresh,
                                certsync.NonceCache(), now=1000)
    with pytest.raises(certsync.CertSyncError, match="bad mac"):  # other path
        certsync.verify_request(SECRET, "/v1/tls/world.next.forkmesh.com", fresh,
                                certsync.NonceCache(), now=1000)
    forged = dict(fresh, **{certsync.NODE_HEADER: "n3"})  # node is signed too
    with pytest.raises(certsync.CertSyncError, match="bad mac"):
        certsync.verify_request(SECRET, "/v1/tls/app.next.forkmesh.com", forged,
                                certsync.NonceCache(), now=1000)
    with pytest.raises(certsync.CertSyncError, match="malformed"):
        certsync.verify_request(SECRET, "/x", {}, certsync.NonceCache())


def test_secret_comes_from_the_credentials_directory(tmp_path):
    # certsync's own credential: the mesh secret (which the app holds) is
    # not read, so it cannot authorize a private-key download.
    (tmp_path / "FORKMESH_MESH_SECRET").write_bytes(SECRET + b"\n")
    with pytest.raises(OSError):
        certsync.read_secret({}, {"CREDENTIALS_DIRECTORY": str(tmp_path)})
    (tmp_path / "FORKMESH_CERTSYNC_SECRET").write_bytes(SECRET + b"\n")
    assert certsync.read_secret({}, {"CREDENTIALS_DIRECTORY": str(tmp_path)}) == SECRET
    with pytest.raises(certsync.CertSyncError):
        certsync.read_secret({}, {})
    (tmp_path / "FORKMESH_CERTSYNC_SECRET").write_bytes(b"short")
    with pytest.raises(certsync.CertSyncError, match="shorter"):
        certsync.read_secret({}, {"CREDENTIALS_DIRECTORY": str(tmp_path)})


DAY = 86400
NOW = 2_000_000_000


def fake_pair(name, tag="A"):
    return ("-----BEGIN CERTIFICATE-----\nCERT %s %s\n-----END CERTIFICATE-----\n"
            % (name, tag),
            "-----BEGIN PRIVATE KEY-----\nKEY %s %s\n-----END PRIVATE KEY-----\n"  # forkmesh-secret-scan:ignore-line
            % (name, tag))


# Fake certificates carry their expiry in the tag: "exp=<unix time>".
def fake_validator(name, cert, key):
    text = cert.decode()
    if "BAD" in text:
        raise certsync.CertSyncError("%s: key does not match certificate" % name)
    if name not in text:
        raise certsync.CertSyncError("%s: certificate does not cover the name" % name)
    for word in text.split():
        if word.startswith("exp="):
            return int(word[4:])
    return NOW + 60 * DAY


def tag(days):
    return "exp=%d" % (NOW + days * DAY)


class Peer:
    """A certsync server (make_handler) holding an export dir."""

    def __init__(self, root, name, pairs):
        import http.server
        self.name = name
        self.export = root / ("export-" + name)
        self.export.mkdir()
        for host, days in pairs.items():
            cert, key = fake_pair(host, tag(days))
            (self.export / (host + ".crt")).write_text(cert)
            (self.export / (host + ".key")).write_text(key)
        config = {"node": name, "names": NAMES, "export_dir": str(self.export),
                  "allowed_peers": ["127.0.0.1"]}
        self.server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), certsync.make_handler(config, SECRET,
                                                    certsync.NonceCache()))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture
def peers(tmp_path):
    started = []

    def start(name, pairs):
        peer = Peer(tmp_path, name, pairs)
        started.append(peer)
        return peer
    yield start
    for peer in started:
        peer.stop()


def node_config(tmp_path, node, peer_urls):
    root = tmp_path / node
    return {"node": node, "names": NAMES, "peers": dict(peer_urls),
            "tls_dir": str(root / "tls"), "include_dir": str(root / "caddy"),
            "export_dir": str(root / "export"),
            "caddy_storage": str(root / "certificates"),
            "challenge_listen": "127.0.0.1:8794", "renew_days": 30}


def issuer_of(name, live):
    return certsync.rendezvous_order(name, live)[0]


def run_sync(config, **kwargs):
    return certsync.sync(config, SECRET, validator=fake_validator,
                         reload=kwargs.pop("reload", []), owner="", now=NOW,
                         **kwargs)


def test_no_fixed_issuer_every_node_has_the_same_config_shape(tmp_path):
    import render
    fleet = render.load_fleet(Path(__file__).resolve().parents[2]
                              / "deploy" / "fleet.example.toml",
                              state_path=tmp_path / "none.json")
    shapes = set()
    for node in ("n1", "n2", "n3"):
        files, manifest = render.render_core(fleet, node)
        config = json.loads(files["/etc/forkmesh-next/certsync.json"][0])
        assert "issuer_url" not in config and "role" not in config
        assert set(config["peers"]) == {"n1", "n2", "n3"}
        assert config["secret_credential"] == "FORKMESH_CERTSYNC_SECRET"
        assert config["max_days"] == 100
        assert manifest["credentials"]["fmnext-certsync.service"] == [
            "FORKMESH_CERTSYNC_SECRET"]
        assert manifest["credentials"]["fmnext-cert-sync.service"] == [
            "FORKMESH_CERTSYNC_SECRET"]
        assert "FORKMESH_CERTSYNC_SECRET" not in manifest["credentials"][
            "forkmesh-app.service"]
        for unit in ("fmnext-certsync.service", "fmnext-cert-sync.service"):
            text = files["/etc/systemd/system/" + unit][0]
            assert "LoadCredentialEncrypted=FORKMESH_MESH_SECRET" not in text
            assert "MemorySwapMax=0" in text
        shapes.add(tuple(sorted(config)))
        assert "fmnext-certsync.service" in manifest["units"]
        assert "fmnext-cert-sync.timer" in manifest["units"]
    assert len(shapes) == 1


def test_sync_pulls_the_freshest_valid_pair_from_any_peer(tmp_path, peers):
    stale = peers("n2", {NAMES[0]: 20, NAMES[1]: 20})
    fresh = peers("n3", {NAMES[0]: 80, NAMES[1]: 50})
    config = node_config(tmp_path, "n1", {"n1": "http://127.0.0.1:1",
                                          "n2": stale.url, "n3": fresh.url})
    reloads = []
    report = run_sync(config, reload=["sh", "-c", "true"])
    assert report["live"] == ["n1", "n2", "n3"]
    for name in NAMES:
        crt = Path(config["tls_dir"]) / (name + ".crt")
        key = Path(config["tls_dir"]) / (name + ".key")
        assert tag(80 if name == NAMES[0] else 50) in crt.read_text()
        assert (key.stat().st_mode & 0o777) == 0o600
        # The freshest pair is > 30 days from expiry: serve it from files,
        # and export it for the peers.
        assert report["modes"][name] == "files"
        assert (Path(config["export_dir"]) / (name + ".crt")).read_text() == \
            crt.read_text()
        include = (Path(config["include_dir"]) / ("tls-%s.caddy" % name)).read_text()
        assert "tls %s/%s.crt" % (config["tls_dir"], name) in include
    http_include = (Path(config["include_dir"]) / "acme-http.caddy").read_text()
    assert "reverse_proxy 127.0.0.1:8794" in http_include
    # Nothing changed on the second round: no rewrite, no reload.
    again = run_sync(config, reload=["false"])
    assert again["changed"] == []
    del reloads


def test_pairs_failing_validation_or_expired_are_ignored(tmp_path, peers):
    bad = peers("n2", {NAMES[0]: 90})
    cert, key = fake_pair(NAMES[0], "BAD " + tag(95))
    (bad.export / (NAMES[0] + ".crt")).write_text(cert)
    expired = peers("n3", {NAMES[0]: -1, NAMES[1]: 40})
    config = node_config(tmp_path, "n1", {"n1": "http://127.0.0.1:1",
                                          "n2": bad.url, "n3": expired.url})
    report = run_sync(config)
    assert not (Path(config["tls_dir"]) / (NAMES[0] + ".crt")).exists()
    assert tag(40) in (Path(config["tls_dir"]) / (NAMES[1] + ".crt")).read_text()
    # No valid pair for app.next anywhere: the rendezvous-first live node
    # issues (acme), any other node serves a local self-signed certificate.
    live = report["live"]
    want = "acme" if issuer_of(NAMES[0], live) == "n1" else "internal"
    assert report["modes"][NAMES[0]] == want


def test_wrong_secret_peer_is_not_live(tmp_path, peers):
    peer = peers("n2", {NAMES[0]: 80, NAMES[1]: 80})
    config = node_config(tmp_path, "n1", {"n1": "http://127.0.0.1:1",
                                          "n2": peer.url})
    report = certsync.sync(config, b"z" * 48, validator=fake_validator,
                           reload=[], owner="", now=NOW)
    assert report["live"] == ["n1"]
    assert not (Path(config["tls_dir"]) / (NAMES[0] + ".crt")).exists()


def test_rendezvous_issuer_renews_and_the_next_node_takes_over(tmp_path, peers):
    nodes = ["n1", "n2", "n3"]
    name = NAMES[0]
    first = issuer_of(name, nodes)
    others = [n for n in nodes if n != first]
    # Everyone holds a pair 10 days from expiry (inside renew_days=30).
    servers = {n: peers(n, {NAMES[0]: 10, NAMES[1]: 10}) for n in others}
    urls = {n: servers[n].url if n in servers else "http://127.0.0.1:1"
            for n in nodes}
    config = node_config(tmp_path, first, urls)
    report = run_sync(config)
    assert report["issuer"][name] == first
    assert report["modes"][name] == "acme"        # renew here, via Caddy
    include = (Path(config["include_dir"]) / ("tls-%s.caddy" % name)).read_text()
    assert "issuer acme" in include
    # Caddy solves this name itself: no forwarding block for it.
    http_include = (Path(config["include_dir"]) / "acme-http.caddy").read_text()
    assert "http://%s" % name not in http_include
    # A non-issuer keeps serving its files while the issuer renews.
    second = others[0]
    urls2 = dict(urls, **{first: "http://127.0.0.1:1"})
    config2 = node_config(tmp_path, second, urls2)
    (Path(config2["tls_dir"])).mkdir(parents=True)
    report2 = run_sync(config2)
    # The issuer (first) is not reachable from here: the next node in
    # rendezvous order among the live ones takes over.
    live2 = report2["live"]
    assert first not in live2
    assert report2["issuer"][name] == issuer_of(name, live2)
    expected = "acme" if issuer_of(name, live2) == second else "files"
    assert report2["modes"][name] == expected
    # Once the renewed pair shows up in the issuer's Caddy storage, the next
    # sync installs and exports it and goes back to files.
    storage = Path(config["caddy_storage"]) / "acme-v02.api.letsencrypt.org-directory" / name
    storage.mkdir(parents=True)
    cert, key = fake_pair(name, tag(90))
    (storage / (name + ".crt")).write_text(cert)
    (storage / (name + ".key")).write_text(key)
    report = run_sync(config)
    assert report["modes"][name] == "files"
    assert tag(90) in (Path(config["export_dir"]) / (name + ".crt")).read_text()


def test_prepare_is_offline_and_lets_caddy_start(tmp_path):
    config = node_config(tmp_path, "n2", {"n1": "http://127.0.0.1:9",
                                          "n2": "http://127.0.0.1:9",
                                          "n3": "http://127.0.0.1:9"})
    report = certsync.sync(config, b"", validator=fake_validator, reload=["false"],
                           owner="", now=NOW, offline=True)
    for name in NAMES:
        assert (Path(config["include_dir"]) / ("tls-%s.caddy" % name)).is_file()
        want = "acme" if issuer_of(name, ["n1", "n2", "n3"]) == "n2" else "internal"
        assert report["modes"][name] == want


def test_challenge_forwarder_tries_peers_in_rendezvous_order():
    config = {"node": "n1", "names": NAMES,
              "challenge_peers": {"n1": "10.77.0.1:80", "n2": "10.77.0.2:80",
                                  "n3": "10.77.0.3:80"}}
    order = certsync.challenge_order(config, NAMES[0])
    assert "10.77.0.1:80" not in order and len(order) == 2
    asked = []

    class Response:
        def __init__(self, status, body):
            self.status, self._body = status, body

        def read(self, limit):
            return self._body

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def opener(request, timeout=None):
        asked.append((request.full_url, request.headers.get("Host"),
                      request.headers.get("X-fmnext-acme-hop")))
        if request.full_url.startswith("http://" + order[0]):
            import urllib.error
            raise urllib.error.HTTPError(request.full_url, 404, "nf", {}, None)
        return Response(200, b"token.thumbprint")
    status, body = certsync.forward_challenge(
        config, NAMES[0], "/.well-known/acme-challenge/abc_DEF-1", opener)
    assert (status, body) == (200, b"token.thumbprint")
    assert [a[0].split("/")[2] for a in asked] == [o for o in order]
    assert all(a[1] == NAMES[0] and a[2] == "n1" for a in asked)
    # Unknown names, other paths and malformed tokens are never forwarded.
    asked.clear()
    for host, path in (("forkmesh.com", "/.well-known/acme-challenge/x"),
                       (NAMES[0], "/api/x"),
                       (NAMES[0], "/.well-known/acme-challenge/../../x")):
        assert certsync.forward_challenge(config, host, path, opener) == (404, b"")
    assert asked == []


def test_forwarded_challenges_are_not_forwarded_again():
    import http.server
    import urllib.request
    config = {"node": "n2", "names": NAMES,
              "challenge_peers": {"n1": "127.0.0.1:9"}}
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", 0), certsync.make_challenge_handler(
            config, opener=lambda *a, **k: pytest.fail("forwarded twice")))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        request = urllib.request.Request(
            "http://127.0.0.1:%d/.well-known/acme-challenge/abc" % server.server_address[1],
            headers={"Host": NAMES[0], certsync.HOP_HEADER: "n1"})
        with pytest.raises(urllib.error.HTTPError) as error:
            urllib.request.urlopen(request, timeout=5)
        assert error.value.code == 404
    finally:
        server.shutdown()
        server.server_close()


def test_response_mac_is_checked(tmp_path):
    class Response:
        headers = {certsync.BODY_MAC_HEADER: "0" * 64}

        def read(self, limit):
            return json.dumps({"name": NAMES[0], "cert": "x", "key": "y"}).encode()

        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False
    with pytest.raises(certsync.CertSyncError, match="MAC"):
        certsync.fetch_pair({"node": "n2"}, SECRET, NAMES[0],
                            opener=lambda *a, **k: Response(),
                            url="http://127.0.0.1:1")


def test_export_copies_the_freshest_local_pair_with_group_read(tmp_path):
    storage = tmp_path / "certificates"
    for issuer_dir, days in (("acme.zerossl.com-v2-dv90", 40),
                             ("acme-v02.api.letsencrypt.org-directory", 85)):
        folder = storage / issuer_dir / NAMES[0]
        folder.mkdir(parents=True)
        cert, key = fake_pair(NAMES[0], tag(days))
        (folder / (NAMES[0] + ".crt")).write_text(cert)
        (folder / (NAMES[0] + ".key")).write_text(key)
    config = {"names": NAMES, "caddy_storage": str(storage),
              "tls_dir": str(tmp_path / "tls"),
              "export_dir": str(tmp_path / "export")}
    changed = certsync.export(config, validator=fake_validator, now=NOW)
    assert changed == [NAMES[0] + ".crt", NAMES[0] + ".key"]
    exported = tmp_path / "export" / (NAMES[0] + ".crt")
    assert tag(85) in exported.read_text()
    assert (exported.stat().st_mode & 0o777) == 0o640
    assert certsync.export(config, validator=fake_validator, now=NOW) == []


def _ossl(*args):
    subprocess.run([OPENSSL, *map(str, args)], check=True, capture_output=True)


def make_ca(tmp_path, label="ca"):
    key, crt = tmp_path / (label + ".key"), tmp_path / (label + ".crt")
    _ossl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
          "-nodes", "-days", 3650, "-subj", "/CN=Test Root " + label,
          "-addext", "basicConstraints=critical,CA:TRUE",
          "-addext", "keyUsage=critical,keyCertSign,cRLSign",
          "-keyout", key, "-out", crt)
    return crt, key


def make_leaf(tmp_path, name, days, ca=None, intermediate=False):
    """(chain pem, key pem): a server certificate for ``name`` issued by
    ``ca`` (through an intermediate when asked), or self-signed."""
    tag = "%s-%d-%s-%s" % (name, days, bool(ca), intermediate)
    key, csr, crt = (tmp_path / (tag + s) for s in (".key", ".csr", ".crt"))
    ext = tmp_path / (tag + ".ext")
    ext.write_text("subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth\n"
                   "keyUsage=critical,digitalSignature\n" % name)
    if ca is None:
        _ossl("req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
              "-nodes", "-days", days, "-subj", "/CN=" + name,
              "-addext", "subjectAltName=DNS:" + name, "-keyout", key, "-out", crt)
        return crt.read_bytes(), key.read_bytes()
    issuer_crt, issuer_key = ca
    chain = b""
    if intermediate:
        ikey, icsr, icrt = (tmp_path / (tag + s) for s in (".ikey", ".icsr", ".icrt"))
        iext = tmp_path / (tag + ".iext")
        iext.write_text("basicConstraints=critical,CA:TRUE,pathlen:0\n"
                        "keyUsage=critical,keyCertSign,cRLSign\n")
        _ossl("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
              "-nodes", "-subj", "/CN=Test Intermediate", "-keyout", ikey, "-out", icsr)
        _ossl("x509", "-req", "-in", icsr, "-CA", issuer_crt, "-CAkey", issuer_key,
              "-CAcreateserial", "-days", 365, "-extfile", iext, "-out", icrt)
        issuer_crt, issuer_key = icrt, ikey
        chain = icrt.read_bytes()
    _ossl("req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
          "-nodes", "-subj", "/CN=" + name, "-keyout", key, "-out", csr)
    _ossl("x509", "-req", "-in", csr, "-CA", issuer_crt, "-CAkey", issuer_key,
          "-CAcreateserial", "-days", days, "-extfile", ext, "-out", crt)
    return crt.read_bytes() + chain, key.read_bytes()


@pytest.mark.skipif(OPENSSL is None, reason="openssl not installed")
def test_openssl_validator(tmp_path):
    ca = make_ca(tmp_path)
    ca_file = ca[0]
    cert, key = make_leaf(tmp_path, NAMES[0], 2, ca, intermediate=True)
    not_after = certsync.openssl_validate(NAMES[0], cert, key, ca_file=ca_file)
    assert time.time() + 86400 < not_after < time.time() + 3 * 86400
    with pytest.raises(certsync.CertSyncError, match="cover"):
        certsync.openssl_validate(NAMES[1], cert, key, ca_file=ca_file)
    _, other_key = make_leaf(tmp_path, "other.example", 2, ca)
    with pytest.raises(certsync.CertSyncError, match="does not match"):
        certsync.openssl_validate(NAMES[0], cert, other_key, ca_file=ca_file)
    # The intermediate is required: the leaf alone does not chain.
    leaf_only = cert.split(b"-----END CERTIFICATE-----")[0] + b"-----END CERTIFICATE-----\n"
    with pytest.raises(certsync.CertSyncError, match="trusted CA"):
        certsync.openssl_validate(NAMES[0], leaf_only, key, ca_file=ca_file)
    certsync.check_pem_shape(NAMES[0], cert, key)
    with pytest.raises(certsync.CertSyncError):
        certsync.check_pem_shape(NAMES[0], key, cert)


@pytest.mark.skipif(OPENSSL is None, reason="openssl not installed")
def test_openssl_validator_refuses_untrusted_and_long_lived_pairs(tmp_path):
    ca = make_ca(tmp_path)
    # A compromised peer's self-signed certificate valid "until 9999" must
    # never win freshest(): it does not chain, and it is too long-lived.
    cert, key = make_leaf(tmp_path, NAMES[0], 2)
    with pytest.raises(certsync.CertSyncError, match="trusted CA"):
        certsync.openssl_validate(NAMES[0], cert, key, ca_file=ca[0])
    # Signed by another CA than the trusted one.
    rogue = make_ca(tmp_path, "rogue")
    cert, key = make_leaf(tmp_path, NAMES[0], 2, rogue)
    with pytest.raises(certsync.CertSyncError, match="trusted CA"):
        certsync.openssl_validate(NAMES[0], cert, key, ca_file=ca[0])
    # Trusted, but expiring more than 100 days out.
    cert, key = make_leaf(tmp_path, NAMES[0], 400, ca)
    with pytest.raises(certsync.CertSyncError, match="100 days"):
        certsync.openssl_validate(NAMES[0], cert, key, ca_file=ca[0])
    # Not valid yet (checked as of an earlier "now").
    cert, key = make_leaf(tmp_path, NAMES[0], 30, ca)
    with pytest.raises(certsync.CertSyncError, match="not valid yet"):
        certsync.openssl_validate(NAMES[0], cert, key, ca_file=ca[0],
                                  now=time.time() - 3 * 86400)
    # The config-bound validator uses the configured trust store.
    validator = certsync.make_validator({"ca_file": str(ca[0]), "max_days": 100})
    assert validator(NAMES[0], cert, key) > time.time()
    # Without the test CA (system store only) the same pair is refused.
    with pytest.raises(certsync.CertSyncError, match="trusted CA"):
        certsync.make_validator({})(NAMES[0], cert, key)


def test_caddy_local_issuer_is_never_collected(tmp_path):
    storage = tmp_path / "certificates"
    for issuer in ("local", "acme-v02.api.letsencrypt.org-directory"):
        directory = storage / issuer / NAMES[0]
        directory.mkdir(parents=True)
        (directory / (NAMES[0] + ".crt")).write_bytes(b"c")
        (directory / (NAMES[0] + ".key")).write_bytes(b"k")
    pairs = certsync.storage_pairs(storage, NAMES[0])
    assert [p[0].parent.parent.name for p in pairs] == [
        "acme-v02.api.letsencrypt.org-directory"]


def test_redirect_include_sends_nosniff():
    config = {"node": "n1", "names": NAMES}
    text = certsync.http_include(config, {NAMES[0]: "files", NAMES[1]: "files"})
    assert "header X-Content-Type-Options nosniff" in text
