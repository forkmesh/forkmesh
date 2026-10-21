import base64
import hashlib
import json
import os
import stat
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "www" / "public" / "install-runner.sh"
TOKEN = "fmrt_" + "ab" * 32


def unb64(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


class App:
    def __init__(self, status=201, owner="alice"):
        self.status = status
        self.owner = owner
        self.bodies = []
        app = self

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                raw = self.rfile.read(int(self.headers["content-length"]))
                body = json.loads(raw)
                app.bodies.append((self.path, body))
                digest = hashlib.sha256(body["token"].encode()).hexdigest()
                message = "\n".join(("forkmesh-runner-register-token-v1",
                                     body["name"], body["publicKey"], digest,
                                     str(body["ts"]))).encode()
                Ed25519PublicKey.from_public_bytes(
                    unb64(body["publicKey"])).verify(unb64(body["proof"]), message)
                answer = ({"ok": True, "runner": {
                    "name": body["name"], "scope": "personal",
                    "allowedOwners": [app.owner], "registeredBy": app.owner,
                    "publicKey": body["publicKey"]}}
                    if app.status < 300 else
                    {"ok": False, "error": "invalid_token",
                     "message": "The setup token is unknown, used or expired."})
                data = json.dumps(answer).encode()
                self.send_response(app.status)
                self.send_header("content-type", "application/json")
                self.send_header("content-length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_args):
                pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = "http://127.0.0.1:%d" % self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()


def sandbox_env(tmp_path):
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    home = tmp_path / "home"
    home.mkdir(exist_ok=True)
    env.update({
        "HOME": str(home),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "FORKMESH_RUNNER_TEST": "1",
        "FORKMESH_RUNNER_PREFIX": str(tmp_path / "root"),
    })
    return env


def fake_binary(tmp_path):
    path = tmp_path / "forkmesh-mirror-node"
    path.write_text("#!/bin/sh\nexit 0\n")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def run(tmp_path, *args, env=None):
    return subprocess.run(["bash", str(SCRIPT), *args], env=env or sandbox_env(tmp_path),
                          capture_output=True, text=True, timeout=120)


def test_arguments_are_validated_before_anything_changes(tmp_path):
    for args in (["--token", "nope", "--name", "box"],
                 ["--token", TOKEN, "--name", "Bad_Name"],
                 ["--token", TOKEN, "--name", "box", "--app", "http://evil.test"],
                 ["--token", TOKEN, "--name", "box", "--providers", "gemini"],
                 ["--token", TOKEN, "--name", "box", "--bogus"]):
        result = run(tmp_path, *args)
        assert result.returncode == 2, (args, result.stderr)
    assert not (tmp_path / "root").exists()
    assert "--token T" in run(tmp_path, "--help").stdout


ALICE_ETC = "etc/forkmesh-runner/accounts/alice"
ALICE_STATE = "var/lib/forkmesh-runner/accounts/alice"


def install(tmp_path, app, name, env=None):
    return run(tmp_path, "--token", TOKEN, "--name", name, "--app", app.url,
               "--binary", str(fake_binary(tmp_path)), env=env)


def read_json(path):
    return json.loads(path.read_text())


def test_registers_with_a_signed_proof_and_writes_a_runner_config(tmp_path):
    app = App()
    try:
        result = install(tmp_path, app, "alice-box")
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    assert TOKEN not in result.stdout + result.stderr
    assert [path for path, _ in app.bodies] == ["/api/runners/register-token"]
    body = app.bodies[0][1]
    assert body["token"] == TOKEN and body["name"] == "alice-box"
    root = tmp_path / "root"
    key = root / ALICE_STATE / "identity/ed25519.pem"
    assert stat.S_IMODE(key.stat().st_mode) == 0o600
    assert not (root / "var/lib/forkmesh-runner/pending-alice-box.pem").exists()
    config_path = root / ALICE_ETC / "runner.json"
    assert stat.S_IMODE(config_path.stat().st_mode) == 0o600
    config = read_json(config_path)
    assert config["role"] == "runner"
    assert config["identityKey"] == "/" + ALICE_STATE + "/identity/ed25519.pem"
    assert config["gatewayConfig"] == "/" + ALICE_STATE + "/gateway/config.json"
    assert config["runner"]["allowedOwners"] == ["alice"]
    assert config["runner"]["appUrl"] == app.url
    assert config["runner"]["jobUser"] == "fmjob-alice"
    assert config["runner"]["workRoot"] == "/" + ALICE_STATE + "/work"
    assert config["listen"].startswith("127.0.0.1:")
    assert config["listen"] != "127.0.0.1:8791"
    assert (root / ALICE_STATE / "work").is_dir()
    assert (root / ALICE_STATE / "home/.claude").is_dir()
    identity = read_json(root / ALICE_STATE / "gateway/config.json")
    assert identity == {"schemaVersion": 1, "node": {
        "name": "alice-box", "publicKey": body["publicKey"]}, "repositories": []}
    unit = (root / "etc/systemd/system/forkmesh-runner@.service").read_text()
    assert "--config /etc/forkmesh-runner/accounts/%i/runner.json" in unit
    assert "ReadWritePaths=/var/lib/forkmesh-runner/accounts/%i\n" in unit
    assert "BindPaths=/var/lib/forkmesh-runner/accounts/%i/tmp:/tmp" in unit
    assert "NoNewPrivileges=true" in unit and "IPAddressDeny=169.254.0.0/16" in unit
    assert not (root / "etc/systemd/system/forkmesh-runner.service").exists()
    nft = (root / "etc/forkmesh-runner/jobs.nft").read_text()
    assert 'meta skuid "fmjob-alice"' in nft and "192.168.0.0/16" in nft
    assert "flush ruleset" not in nft
    assert nft.endswith('include "/etc/forkmesh-runner/jobs.d/*.nft"\n')
    assert (root / "etc/forkmesh-runner/jobs.d").is_dir()
    assert TOKEN not in json.dumps(config) and TOKEN not in unit
    assert (root / "usr/local/bin/forkmesh-mirror-node").is_file()
    assert "forkmesh-runner@alice.service" in result.stdout


def test_a_second_run_keeps_the_key_and_a_refusal_is_reported(tmp_path):
    app = App()
    try:
        install(tmp_path, app, "alice-box")
        first = app.bodies[-1][1]["publicKey"]
        listen = read_json(tmp_path / "root" / ALICE_ETC / "runner.json")["listen"]
        again = install(tmp_path, app, "alice-box")
        assert again.returncode == 0, again.stderr
        assert app.bodies[-1][1]["publicKey"] == first
        assert "Reusing the runner key" in again.stdout
        assert read_json(tmp_path / "root" / ALICE_ETC / "runner.json")["listen"] == listen
        app.status = 401
        refused = install(tmp_path, app, "alice-box")
    finally:
        app.close()
    assert refused.returncode == 1
    assert "HTTP 401" in refused.stderr and "unknown, used or expired" in refused.stderr


def test_several_accounts_run_side_by_side(tmp_path):
    root = tmp_path / "root"
    app = App(owner="alice")
    try:
        assert install(tmp_path, app, "alice-box").returncode == 0
        alice_config = (root / ALICE_ETC / "runner.json").read_text()
        alice_key = (root / ALICE_STATE / "identity/ed25519.pem").read_bytes()
        app.owner = "bob"
        result = install(tmp_path, app, "bob-box")
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    assert (root / ALICE_ETC / "runner.json").read_text() == alice_config
    assert (root / ALICE_STATE / "identity/ed25519.pem").read_bytes() == alice_key
    bob = read_json(root / "etc/forkmesh-runner/accounts/bob/runner.json")
    alice = json.loads(alice_config)
    assert bob["runner"]["allowedOwners"] == ["bob"]
    assert bob["runner"]["jobUser"] == "fmjob-bob"
    assert bob["runner"]["workRoot"] == "/var/lib/forkmesh-runner/accounts/bob/work"
    assert bob["identityKey"] == "/var/lib/forkmesh-runner/accounts/bob/identity/ed25519.pem"
    assert (root / "var/lib/forkmesh-runner/accounts/bob/identity/ed25519.pem").read_bytes() != alice_key
    ports = {alice["listen"], alice["gatewayProbe"], bob["listen"], bob["gatewayProbe"]}
    assert len(ports) == 4
    assert (root / "var/lib/forkmesh-runner/accounts/bob/home/.claude").is_dir()
    nft = (root / "etc/forkmesh-runner/jobs.nft").read_text()
    assert 'meta skuid "fmjob-alice"' in nft and 'meta skuid "fmjob-bob"' in nft
    assert "forkmesh-runner@bob.service" in result.stdout


def test_the_same_account_renaming_its_runner_keeps_one_instance(tmp_path):
    root = tmp_path / "root"
    app = App(owner="alice")
    try:
        assert install(tmp_path, app, "alice-box").returncode == 0
        old_key = (root / ALICE_STATE / "identity/ed25519.pem").read_bytes()
        (root / ALICE_STATE / "work/.accounts").mkdir()
        result = install(tmp_path, app, "alice-new")
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    assert "Kept the key of runner alice-box" in result.stdout
    assert (root / ALICE_STATE / "identity/ed25519.pem.alice-box").read_bytes() == old_key
    assert (root / ALICE_STATE / "identity/ed25519.pem").read_bytes() != old_key
    assert app.bodies[-1][1]["publicKey"] != app.bodies[0][1]["publicKey"]
    identity = read_json(root / ALICE_STATE / "gateway/config.json")
    assert identity["node"]["name"] == "alice-new"
    assert (root / ALICE_STATE / "work/.accounts").is_dir()
    assert sorted(p.name for p in (root / "etc/forkmesh-runner/accounts").iterdir()) == ["alice"]


def write_legacy_runner(root, owner, name):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    state = root / "var/lib/forkmesh-runner"
    etc = root / "etc/forkmesh-runner"
    for folder in (state / "identity", state / "gateway", state / "work", etc):
        folder.mkdir(parents=True, exist_ok=True)
    pem = Ed25519PrivateKey.generate().private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption())
    (state / "identity/ed25519.pem").write_bytes(pem)
    (state / "gateway/config.json").write_text(json.dumps(
        {"schemaVersion": 1, "node": {"name": name, "publicKey": "x"},
         "repositories": []}))
    (etc / "runner.json").write_text(json.dumps({
        "schemaVersion": 1, "role": "runner", "listen": "127.0.0.1:8795",
        "gatewayProbe": "127.0.0.1:8796",
        "gatewayConfig": "/var/lib/forkmesh-runner/gateway/config.json",
        "identityKey": "/var/lib/forkmesh-runner/identity/ed25519.pem",
        "runner": {"appUrl": "https://app.forkmesh.com", "allowedOwners": [owner],
                   "workRoot": "/var/lib/forkmesh-runner/work",
                   "jobUser": "forkmesh-job"}}))
    return pem


def test_an_older_single_runner_install_keeps_its_layout_next_to_new_ones(tmp_path):
    root = tmp_path / "root"
    pem = write_legacy_runner(root, "alice", "alice-box")
    app = App(owner="alice")
    try:
        again = install(tmp_path, app, "alice-box")
        app.owner = "bob"
        bob_result = install(tmp_path, app, "bob-box")
    finally:
        app.close()
    assert again.returncode == 0, again.stderr
    assert "Reusing the runner key" in again.stdout
    assert (root / "var/lib/forkmesh-runner/identity/ed25519.pem").read_bytes() == pem
    legacy = read_json(root / "etc/forkmesh-runner/runner.json")
    assert legacy["runner"]["jobUser"] == "forkmesh-job"
    assert legacy["runner"]["workRoot"] == "/var/lib/forkmesh-runner/work"
    assert legacy["listen"] == "127.0.0.1:8795"
    unit = (root / "etc/systemd/system/forkmesh-runner.service").read_text()
    assert "--config /etc/forkmesh-runner/runner.json" in unit
    assert "ReadWritePaths=/var/lib/forkmesh-runner /var/lib/forkmesh-job" in unit
    assert "InaccessiblePaths=-/var/lib/forkmesh-runner/accounts" in unit
    assert not (root / ALICE_ETC).exists()
    assert bob_result.returncode == 0, bob_result.stderr
    bob = read_json(root / "etc/forkmesh-runner/accounts/bob/runner.json")
    assert bob["listen"] not in ("127.0.0.1:8795", "127.0.0.1:8796")
    assert read_json(root / "etc/forkmesh-runner/runner.json") == legacy
    nft = (root / "etc/forkmesh-runner/jobs.nft").read_text()
    assert 'meta skuid "forkmesh-job"' in nft and 'meta skuid "fmjob-bob"' in nft


def test_a_job_user_another_runner_holds_is_refused(tmp_path):
    root = tmp_path / "root"
    app = App(owner="carol")
    try:
        assert install(tmp_path, app, "carol-box").returncode == 0
        path = root / "etc/forkmesh-runner/accounts/carol/runner.json"
        config = read_json(path)
        config["runner"]["jobUser"] = "fmjob-bob"
        path.write_text(json.dumps(config))
        app.owner = "bob"
        result = install(tmp_path, app, "bob-box")
    finally:
        app.close()
    assert result.returncode == 1
    assert "fmjob-bob already belongs to the runner of carol" in result.stderr
    assert not (root / "etc/forkmesh-runner/accounts/bob/runner.json").exists()


def test_a_long_account_name_gets_a_short_unique_job_user(tmp_path):
    owner = "a" + "b" * 40
    app = App(owner=owner)
    try:
        result = install(tmp_path, app, "long-box")
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    digest = hashlib.sha256(owner.encode()).hexdigest()[:6]
    user = "fmjob-" + owner[:19] + "-" + digest
    assert len(user) == 32
    config = read_json(tmp_path / "root/etc/forkmesh-runner/accounts" / owner / "runner.json")
    assert config["runner"]["jobUser"] == user


def test_the_script_deletes_nothing_recursively_and_never_flushes_the_firewall():
    text = SCRIPT.read_text()
    assert "rm -rf" not in text and "rm -r " not in text
    assert "flush ruleset" not in text
    assert "--uninstall" not in text and "--purge" not in text
    assert text.rstrip().endswith("}")


def test_macos_installs_a_launch_daemon_and_a_pf_anchor(tmp_path):
    app = App()
    try:
        env = sandbox_env(tmp_path)
        env["FORKMESH_RUNNER_PLATFORM"] = "Darwin"
        result = install(tmp_path, app, "alice-mac", env=env)
        app.owner = "bob"
        second = install(tmp_path, app, "bob-mac", env=env)
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    assert second.returncode == 0, second.stderr
    root = tmp_path / "root"
    state = "/private/var/db/forkmesh-runner/accounts/alice"
    config = read_json(root / "private/etc/forkmesh-runner/accounts/alice/runner.json")
    assert config["identityKey"] == state + "/identity/ed25519.pem"
    assert config["runner"]["workRoot"] == state + "/work"
    assert (root / state.lstrip("/") / "home/.claude").is_dir()
    assert not (root / "etc/systemd").exists() and not (root / "var/lib").exists()
    plist = (root / "Library/LaunchDaemons/com.forkmesh.runner.alice.plist").read_text()
    assert "<string>com.forkmesh.runner.alice</string>" in plist
    assert "<string>/private/etc/forkmesh-runner/accounts/alice/runner.json</string>" in plist
    assert "<string>%s/tmp</string>" % state in plist
    assert "/private/var/log/forkmesh-runner-alice.log" in plist
    assert str(tmp_path) not in plist and TOKEN not in plist
    assert (root / "Library/LaunchDaemons/com.forkmesh.runner.bob.plist").is_file()
    pf = (root / "private/etc/forkmesh-runner/jobs.pf").read_text()
    assert "192.168.0.0/16" in pf and "user %d" % os.getuid() in pf
    assert pf.count("192.168.0.0/16") == 2
    assert stat.S_IMODE((root / "private/etc/forkmesh-runner/jobs.pf").stat().st_mode) == 0o600
    firewall = (root / "Library/LaunchDaemons/com.forkmesh.runner-firewall.plist").read_text()
    assert "pfctl -a com.apple/forkmesh-runner -f /private/etc/forkmesh-runner/jobs.pf" in firewall
    assert "pfctl -F" not in firewall and "pf.conf" not in firewall




def preregistered(tmp_path, *extra, name="forkmesh-runner", owner="acme", env=None):
    return run(tmp_path, "--preregistered", "--name", name, "--owner", owner,
               "--app", "https://code.example.com", "--providers", "none",
               "--binary", str(fake_binary(tmp_path)), *extra, env=env)


def test_preregistered_mode_skips_the_handshake_and_leases_actions(tmp_path):
    result = preregistered(tmp_path)
    assert result.returncode == 0, result.stderr
    root = tmp_path / "root"
    config = read_json(root / "etc/forkmesh-runner/accounts/acme/runner.json")
    assert config["role"] == "runner"
    assert config["enableActions"] is True
    assert config["runner"]["allowedOwners"] == ["acme"]
    assert config["runner"]["appUrl"] == "https://code.example.com"
    assert config["runner"]["jobUser"] == "forkmesh-job"
    identity = read_json(root / "var/lib/forkmesh-runner/accounts/acme/gateway/config.json")
    assert identity["node"]["name"] == "forkmesh-runner"
    key = identity["node"]["publicKey"]
    assert len(key) == 43 and len(unb64(key)) == 32
    assert result.stdout.splitlines()[-1] == "FM_RUNNER_PUBLIC_KEY=" + key
    assert "Registering" not in result.stdout
    nft = (root / "etc/forkmesh-runner/jobs.nft").read_text()
    assert 'meta skuid "forkmesh-job"' in nft
    again = preregistered(tmp_path)
    assert again.returncode == 0, again.stderr
    assert "Reusing the runner key" in again.stdout
    assert again.stdout.splitlines()[-1] == "FM_RUNNER_PUBLIC_KEY=" + key


def test_a_token_runner_has_no_actions_flag(tmp_path):
    app = App()
    try:
        assert install(tmp_path, app, "alice-box").returncode == 0
    finally:
        app.close()
    assert "enableActions" not in read_json(tmp_path / "root" / ALICE_ETC / "runner.json")


def test_preregistered_arguments_are_checked(tmp_path):
    binary = str(fake_binary(tmp_path))
    for args in (
        ["--preregistered", "--name", "box", "--binary", binary],
        ["--preregistered", "--name", "box", "--owner", "Bad", "--binary", binary],
        ["--preregistered", "--name", "box", "--owner", "acme"],
        ["--preregistered", "--name", "box", "--owner", "acme", "--binary", binary,
         "--token", TOKEN],
        ["--preregistered", "--name", "box", "--owner", "acme", "--binary", binary,
         "--job-user", "root"],
        ["--token", TOKEN, "--name", "box", "--owner", "acme"],
    ):
        result = run(tmp_path, *args)
        assert result.returncode == 2, (args, result.stderr)
    assert not (tmp_path / "root").exists()


def test_preregistered_mode_reuses_the_installed_binary(tmp_path):
    env = sandbox_env(tmp_path)
    binary = tmp_path / "root/usr/local/bin/forkmesh-mirror-node"
    binary.parent.mkdir(parents=True)
    binary.write_text("#!/bin/sh\nexit 0\n")
    binary.chmod(0o755)
    result = run(tmp_path, "--preregistered", "--name", "forkmesh-runner",
                 "--owner", "acme", "--app", "https://code.example.com",
                 "--providers", "none", "--binary", str(binary), env=env)
    assert result.returncode == 0, result.stderr
    assert "Using the installed" in result.stdout
