"""Caddyfile checks against a real Caddy binary, when one is available.

Uses $CADDY, ~/projects/forkmesh/data/fleet/bin/caddy or `caddy` on PATH (the official
v2.6.2 static binary, the same version as Debian 13's package). Nothing is
installed; without a binary these tests skip.

* every rendered Caddyfile adapts with every certificate mode certsync can
  choose (files, acme, internal) and its HTTP-01 forwarding include;
* a live run on loopback (HTTP only, test ports) proves the routing: the
  run_worker_first split, static files with _headers and cache rules, the
  world origin's App assets, Cf-* stripping, the per-node gateway and the
  /_app fallback.
"""

import http.client
import http.server
import json
import shutil
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest

import caddy_routes
import certsync
import next_git_host
import render

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"


def caddy_binary():
    import forkmesh_paths
    return forkmesh_paths.resolve().fleet_binary("caddy", "CADDY")


CADDY = caddy_binary()
GIT_PASSWORD = "test-git-password_0123456789-abcdef"  # forkmesh-secret-scan:ignore-line
needs_caddy = pytest.mark.skipif(CADDY is None, reason="no caddy binary available")


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def rewrite_imports(caddyfile: str, root: Path) -> str:
    return caddyfile.replace("/etc/forkmesh-next/caddy/",
                             str(root / "etc/forkmesh-next/caddy") + "/")


def write_certsync_includes(root: Path, node, modes):
    """What fmnext-certsync writes on a node, under a test root."""
    config = json.loads((root / "etc/forkmesh-next/certsync.json").read_text())
    config["include_dir"] = str(root / "etc/forkmesh-next/caddy")
    config["tls_dir"] = str(root / "tls")
    certsync.write_includes(config, {n: modes for n in config["names"]}
                            if isinstance(modes, str) else modes)
    return config


@needs_caddy
@pytest.mark.parametrize("node", ["n1", "n2", "n3"])
@pytest.mark.parametrize("modes", ["files", "acme", "internal", "mixed"])
def test_rendered_caddyfile_adapts(tmp_path, node, modes):
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    files, _ = render.render_core(fleet, node)
    render.write_tree(files, {}, tmp_path)
    if modes == "mixed":
        modes = {fleet["app_host"]: "acme", fleet["world_host"]: "files"}
    write_certsync_includes(tmp_path, node, modes)
    caddyfile = (tmp_path / "etc/caddy/Caddyfile").read_text()
    assert "acme_issuer" not in caddyfile and "10.77.0.1:80" not in caddyfile
    config = tmp_path / "Caddyfile.test"
    config.write_text(rewrite_imports(caddyfile, tmp_path))
    completed = subprocess.run(
        [CADDY, "adapt", "--config", str(config), "--adapter", "caddyfile"],
        capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    adapted = json.loads(completed.stdout)
    assert adapted["apps"]["http"]["servers"]
    text = completed.stdout
    if modes == "files":
        assert '"load_files"' in text or "load_files" in text
    # Challenges for names Caddy is not solving itself go to the forwarder.
    if modes != "acme":
        assert '"127.0.0.1:8794"' in text
    else:
        assert "127.0.0.1:8794" not in text


# --------------------------------------------------------------------------
# live routing


class Echo(http.server.BaseHTTPRequestHandler):
    tag = "app"

    def log_message(self, *args):
        pass

    def _answer(self):
        length = int(self.headers.get("content-length") or 0)
        if length:
            self.rfile.read(length)
        body = json.dumps({
            "tag": self.tag, "path": self.path, "method": self.command,
            "headers": {k.lower(): v for k, v in self.headers.items()},
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_POST = do_HEAD = _answer


def start_echo(tag):
    handler = type("Echo_" + tag, (Echo,), {"tag": tag})
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def make_dists(root: Path):
    app = root / "app" / "dist"
    world = root / "world" / "dist"
    files = {
        app / "login.html": "LOGIN PAGE",
        app / "app.js": "APP JS",
        app / "other.js": "OTHER JS",
        app / "plain.css": "PLAIN CSS " * 20,
        app / "dashboard" / "index.html": "DASHBOARD",
        app / "assets" / "logo.png": "LOGO",
        app / "_headers": "/*\n  X-Test-Global: yes\n\n/app.js\n  Cache-Control: no-store\n",
        world / "world" / "index.html": "WORLD INDEX",
        world / "world" / "world.js": "WORLD JS",
        world / "assets" / "bot-avatars" / "a.png": "AVATAR",
        world / "_headers": (
            "/*\n  X-Test-World: yes\n\n"
            "/world/*\n  Cache-Control: no-store, max-age=0, must-revalidate\n"
        ),
    }
    for path, text in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    import gzip
    (app / "plain.css.gz").write_bytes(gzip.compress((app / "plain.css").read_bytes()))
    return app, world


@pytest.fixture
def live(tmp_path):
    app_echo = start_echo("app")
    gateway_echo = start_echo("gateway")
    http_port = free_port()
    current = tmp_path / "current"
    app_dist, world_dist = make_dists(current)
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    fleet["paths"] = {"etc": str(tmp_path / "etc/forkmesh-next"),
                      "current": str(current), "tls": str(tmp_path / "tls")}
    APP, WORLD = fleet["app_host"], fleet["world_host"]
    fleet["app"]["port"] = app_echo.server_address[1]
    fleet["app"]["gateway_port"] = gateway_echo.server_address[1]
    files, _ = render.render_core(fleet, "n1")
    caddyfile = files["/etc/caddy/Caddyfile"][0]
    (tmp_path / "etc/forkmesh-next/caddy").mkdir(parents=True)
    (tmp_path / "etc/forkmesh-next/caddy/app-routes.caddy").write_text(
        caddy_routes.generate("app", REPO, dist=app_dist, root=str(app_dist)))
    (tmp_path / "etc/forkmesh-next/caddy/world-routes.caddy").write_text(
        caddy_routes.generate("world", REPO, dist=world_dist, root=str(world_dist),
                              app_root=str(app_dist), app_dist=app_dist))
    # Loopback-only variant: no global TLS/ACME, test port, http:// sites.
    # Loopback: no TLS directives (comment-only includes, which must adapt),
    # and the HTTP-01 include rewritten to a test name, forwarding to an echo.
    for host in (APP, WORLD):
        (tmp_path / ("etc/forkmesh-next/caddy/tls-%s.caddy" % host)).write_text(
            "# test: plain http\n")
    challenge_config = {"node": "n1", "names": ["acme.test:%d" % http_port],
                        "challenge_listen": "127.0.0.1:%d" % gateway_echo.server_address[1]}
    (tmp_path / "etc/forkmesh-next/caddy/acme-http.caddy").write_text(
        certsync.http_include(challenge_config, {}))
    # The private Git mirror (tools/next_git_host.py): a bare repo and the
    # snippet, with a hash made the way setup makes it (password on stdin).
    git_root = tmp_path / "git"
    subprocess.run(["git", "init", "-q", "--bare", "--initial-branch=main",
                    str(git_root / "forkmesh-next.git")],
                   check=True)
    subprocess.run(["git", "-C", str(git_root / "forkmesh-next.git"),
                    "update-server-info"], check=True)
    password_hash = subprocess.run([CADDY, "hash-password"], input=GIT_PASSWORD + "\n",
                                   capture_output=True, text=True, check=True).stdout.strip()
    (tmp_path / "etc/forkmesh-next/caddy/git-private.caddy").write_text(
        next_git_host.snippet(password_hash, root=str(git_root)))
    body = caddyfile[caddyfile.index("(client_headers)"):]
    for host in ("app.next.forkmesh.com", "world.next.forkmesh.com",
                 "n1.next.forkmesh.com"):
        body = body.replace("\n%s {" % host, "\nhttp://%s:%d {" % (host, http_port))
    config = tmp_path / "Caddyfile.live"
    config.write_text("{\n\tadmin off\n\tauto_https off\n\thttp_port %d\n}\n\n%s"
                      % (http_port, body))
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

    def request(host, path, method="GET", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", http_port, timeout=10)
        connection.request(method, path, body=b"x" if method == "POST" else None,
                           headers={"Host": host, **(headers or {})})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response, data

    yield request
    process.terminate()
    process.wait(timeout=10)
    app_echo.shutdown()
    gateway_echo.shutdown()


APP = "app.next.forkmesh.com"
WORLD = "world.next.forkmesh.com"
NODE = "n1.next.forkmesh.com"


def upstream(data):
    return json.loads(data)


@needs_caddy
def test_live_app_routing(live):
    # Worker-first API: to the App, with client address claims replaced.
    response, data = live(APP, "/api/ping", headers={
        "Cf-Connecting-Ip": "6.6.6.6", "Cf-Ray": "abc", "Cf-Ipcountry": "XX",
        "X-Real-Ip": "6.6.6.6",
        "X-Forkmesh-Edge-Client": "v1,203.0.113.7,US,1,mac"})
    seen = upstream(data)
    assert seen["tag"] == "app" and seen["path"] == "/api/ping"
    assert seen["headers"]["cf-connecting-ip"] == "127.0.0.1"
    assert "cf-ray" not in seen["headers"] and "cf-ipcountry" not in seen["headers"]
    assert "x-real-ip" not in seen["headers"]
    assert seen["headers"]["x-forkmesh-edge-client"] == "v1,203.0.113.7,US,1,mac"
    assert seen["headers"]["host"] == APP

    # Static document (drop-trailing-slash style), with _headers applied.
    response, data = live(APP, "/login")
    assert data == b"LOGIN PAGE"
    assert response.getheader("X-Test-Global") == "yes"
    assert response.getheader("Cache-Control") == "public, max-age=0, must-revalidate"
    # Content-stamped assets are immutable ...
    response, data = live(APP, "/other.js?v=1234abcd")
    assert data == b"OTHER JS"
    assert response.getheader("Cache-Control") == "public, max-age=31536000, immutable"
    # ... unless _headers says otherwise (it wins, as on Cloudflare).
    response, data = live(APP, "/app.js?v=1234abcd")
    assert response.getheader("Cache-Control") == "no-store"
    # Negated worker-first page is served statically; the rest of the tree is not.
    response, data = live(APP, "/dashboard")
    assert data == b"DASHBOARD"
    response, data = live(APP, "/dashboard/anything")
    assert upstream(data)["path"] == "/dashboard/anything"
    # Missing files, writes and trailing-slash forms go to the App.
    assert upstream(live(APP, "/missing.js")[1])["tag"] == "app"
    assert upstream(live(APP, "/login", method="POST")[1])["method"] == "POST"
    assert upstream(live(APP, "/login/")[1])["path"] == "/login/"
    # Precompressed sidecars are used.
    response, data = live(APP, "/plain.css", headers={"Accept-Encoding": "gzip"})
    assert response.getheader("Content-Encoding") == "gzip"
    # Host-internal and mesh paths are never public.
    for path in ("/_host/cluster", "/_do/x/y", "/_rpc/x"):
        assert live(APP, path)[0].status == 404


@needs_caddy
def test_live_world_routing(live):
    response, data = live(WORLD, "/")
    assert data == b"WORLD INDEX"
    assert response.getheader("Cache-Control").startswith("no-store")
    assert live(WORLD, "/world/")[1] == b"WORLD INDEX"
    # /health/mobile is answered by the App host (world_health.py) under the
    # World's own Host, so it can say x-forkmesh-worker: world.
    seen = upstream(live(WORLD, "/health/mobile")[1])
    assert seen["tag"] == "app" and seen["path"] == "/health/mobile"
    assert seen["headers"]["host"] == WORLD
    # App-owned assets come from the App's dist, else from the App (App host).
    assert live(WORLD, "/assets/logo.png")[1] == b"LOGO"
    seen = upstream(live(WORLD, "/assets/missing.png")[1])
    assert seen["headers"]["host"] == APP
    # World's own static files (negated worker-first) come from world dist.
    response, data = live(WORLD, "/assets/bot-avatars/a.png")
    assert data == b"AVATAR" and response.getheader("X-Test-World") == "yes"
    response, data = live(WORLD, "/world/world.js")
    assert data == b"WORLD JS"
    assert response.getheader("Cache-Control").startswith("no-store")
    response, data = live(WORLD, "/world/world.js?v=1234abcd")
    assert data == b"WORLD JS"
    assert response.getheader("Cache-Control") == "public, max-age=31536000, immutable"
    # Everything else reaches the App under the world host.
    seen = upstream(live(WORLD, "/chat")[1])
    assert seen["path"] == "/chat" and seen["headers"]["host"] == WORLD


@needs_caddy
def test_live_acme_challenge_forwarding(live):
    # certsync's acme-http include: challenge paths go to the forwarder,
    # everything else on port 80 is redirected to https.
    seen = upstream(live("acme.test", "/.well-known/acme-challenge/tok_en-1")[1])
    assert seen["tag"] == "gateway"
    assert seen["path"] == "/.well-known/acme-challenge/tok_en-1"
    response, _ = live("acme.test", "/other")
    assert response.status == 308


@needs_caddy
def test_live_node_routing(live):
    seen = upstream(live(NODE, "/readyz")[1])
    assert seen["tag"] == "app" and seen["path"] == "/readyz"
    seen = upstream(live(NODE, "/_app/api/version?x=1")[1])
    assert seen["tag"] == "app" and seen["path"] == "/api/version?x=1"
    assert seen["headers"]["host"] == APP
    seen = upstream(live(NODE, "/forkmesh/forkmesh/info/refs?service=git-upload-pack",
                         headers={"Cf-Connecting-Ip": "6.6.6.6"})[1])
    assert seen["tag"] == "gateway"
    assert "cf-connecting-ip" not in seen["headers"]
    assert live(NODE, "/_host/cluster")[0].status == 404
    seen = upstream(live(
        NODE, "/forkmesh/forkmesh.git/info/refs?service=git-receive-pack")[1])
    assert seen["tag"] == "app" and seen["headers"]["host"] == APP
    assert seen["path"] == (
        "/forkmesh/forkmesh.git/info/refs?service=git-receive-pack")
    seen = upstream(live(
        NODE, "/forkmesh/forkmesh.git/git-receive-pack", method="POST")[1])
    assert seen["tag"] == "app" and seen["method"] == "POST"
    seen = upstream(live(NODE, "/forkmesh/forkmesh.git/git-upload-pack",
                         method="POST")[1])
    assert seen["tag"] == "gateway"


@needs_caddy
def test_real_headers_file_adapts(tmp_path):
    # app/tools/build_site_assets.py writes www/public/_headers plus the
    # worker marker into every dist; the generated routes must adapt with it.
    dist = tmp_path / "dist"
    dist.mkdir()
    (dist / "_headers").write_text((REPO / "www/public/_headers").read_text()
                                   + "\n/*\n  x-forkmesh-worker: app\n")
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    files, _ = render.render_core(fleet, "n2")
    render.write_tree(files, {}, tmp_path)
    write_certsync_includes(tmp_path, "n2", "files")
    (tmp_path / "etc/forkmesh-next/caddy/app-routes.caddy").write_text(
        caddy_routes.generate("app", REPO, dist=dist, root=str(dist)))
    (tmp_path / "etc/forkmesh-next/caddy/world-routes.caddy").write_text(
        caddy_routes.generate("world", REPO, dist=dist, root=str(dist),
                              app_root=str(dist), app_dist=dist))
    config = tmp_path / "Caddyfile.test"
    config.write_text(rewrite_imports((tmp_path / "etc/caddy/Caddyfile").read_text(),
                                      tmp_path))
    completed = subprocess.run(
        [CADDY, "adapt", "--config", str(config), "--adapter", "caddyfile"],
        capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    assert "frame-ancestors 'self'" in completed.stdout


@needs_caddy
def test_live_private_git_needs_auth(live):
    import base64
    good = "Basic " + base64.b64encode(("fmnext-ci:" + GIT_PASSWORD).encode()).decode()
    bad = "Basic " + base64.b64encode(b"fmnext-ci:wrong").decode()
    refs = "/git/forkmesh-next.git/info/refs"
    for host in (APP, NODE):
        # Anonymous or wrong: 401 for every path under /git/, never the App,
        # the gateway or a file.
        for path in (refs, "/git/", "/git/forkmesh-next.git/HEAD",
                     "/git/forkmesh-next.git/config", "/git/%2e%2e/git/x",
                     "/git/forkmesh-next.git/objects/info/packs"):
            for headers in ({}, {"Authorization": bad}):
                response, data = live(host, path, headers=headers)
                assert response.status == 401, (host, path, response.status)
                assert b"tag" not in data
        response, _ = live(host, refs, method="POST")
        assert response.status == 401
        # Dot segments are cleaned first: this is /etc/passwd, not a file
        # under the Git root, and it goes on to the site's normal routes.
        response, data = live(host, "/git/../etc/passwd")
        assert b"root:" not in data and response.status != 401
        # With the password: the dumb-HTTP files are served, never cached.
        response, data = live(host, "/git/forkmesh-next.git/HEAD",
                              headers={"Authorization": good})
        assert response.status == 200 and data == b"ref: refs/heads/main\n"
        assert response.getheader("Cache-Control") == "no-store"
        response, _ = live(host, refs, headers={"Authorization": good})
        assert response.status == 200
        # No directory listings.
        response, data = live(host, "/git/forkmesh-next.git/",
                              headers={"Authorization": good})
        assert response.status == 404
    # world.next has no /git/ route: the request goes on to the App.
    seen = upstream(live(WORLD, refs)[1])
    assert seen["tag"] == "app"


def run_access_log_site(tmp_path, ranges, headers):
    port = free_port()
    snippet = "\n".join(render.access_log_snippet())
    if ranges is not None:
        snippet = snippet.replace(
            " ".join(render.CLOUDFLARE_RANGES), " ".join(ranges))
    config = tmp_path / "Caddyfile.access"
    config.write_text(
        "{\n\tadmin off\n\tauto_https off\n}\n\n%s\n"
        "(client_headers) {\n\trequest_header -Cf-*\n"
        "\trequest_header -X-Forkmesh-Edge-Where\n}\n\n"
        "http://127.0.0.1:%d {\n\timport access_log\n\timport client_headers\n"
        "\theader X-Forkmesh-Expected-Degraded 1\n"
        "\trespond \"seen={http.request.header.Cf-Connecting-Ip}"
        "{http.request.header.X-Forkmesh-Edge-Where}\" 200\n}\n"
        % (snippet, port))
    log = tmp_path / "caddy.log"
    with log.open("wb") as sink:
        process = subprocess.Popen([CADDY, "run", "--config", str(config),
                                    "--adapter", "caddyfile"],
                                   stdout=sink, stderr=sink)
        try:
            deadline = time.time() + 15
            while time.time() < deadline and process.poll() is None:
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.1)
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            connection.request("GET", "/x", headers=headers)
            body = connection.getresponse().read()
            connection.close()
            deadline = time.time() + 5
            while time.time() < deadline and b"http.log.access" not in log.read_bytes():
                time.sleep(0.1)
        finally:
            process.terminate()
            process.wait(timeout=10)
    for line in log.read_text().splitlines():
        if "http.log.access" in line:
            return body, json.loads(line)
    pytest.fail("no access line: %s" % log.read_text()[-2000:])


@needs_caddy
def test_access_log_names_the_cloudflare_visitor(tmp_path):
    headers = {"Cf-Connecting-Ip": "203.0.113.7", "Cf-Ipcountry": "DE",
               "Cf-Region": "Bavaria", "Cf-Ipcity": "Munich"}
    body, entry = run_access_log_site(tmp_path, ["127.0.0.0/8"], headers)
    assert body == b"seen="
    assert entry["cf_ip"] == "203.0.113.7"
    assert entry["cf_country"] == "DE"
    assert entry["cf_region"] == "Bavaria"
    assert entry["cf_city"] == "Munich"
    assert entry["expected_degraded"] == "1"
    assert "headers" not in entry["request"]


@needs_caddy
def test_access_log_ignores_cloudflare_headers_from_others(tmp_path):
    headers = {"Cf-Connecting-Ip": "203.0.113.7", "Cf-Ipcountry": "DE"}
    body, entry = run_access_log_site(tmp_path, None, headers)
    assert body == b"seen="
    assert not entry.get("cf_ip") and not entry.get("cf_country")
    assert entry["request"]["client_ip"] == "127.0.0.1"


@needs_caddy
def test_access_log_names_the_visitor_the_worker_relays(tmp_path):
    headers = {"X-Forkmesh-Edge-Client": "v1,2001:db8::7,CA,1790000000,mac",
               "X-Forkmesh-Edge-Where": "city=Toronto&region=Ontario&asn=812"}
    body, entry = run_access_log_site(tmp_path, ["127.0.0.0/8"], headers)
    assert body == b"seen="
    assert entry["edge_ip"] == "2001:db8::7"
    assert entry["edge_country"] == "CA"
    assert entry["edge_where"] == "city=Toronto&region=Ontario&asn=812"
    assert not entry.get("cf_ip")


@needs_caddy
def test_access_log_ignores_a_relay_header_from_others(tmp_path):
    headers = {"X-Forkmesh-Edge-Client": "v1,2001:db8::7,CA,1790000000,mac",
               "X-Forkmesh-Edge-Where": "city=Toronto"}
    body, entry = run_access_log_site(tmp_path, None, headers)
    assert body == b"seen="
    assert not entry.get("edge_ip") and not entry.get("edge_where")
