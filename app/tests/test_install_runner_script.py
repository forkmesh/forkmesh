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
    def __init__(self, status=201):
        self.status = status
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
                    "allowedOwners": ["alice"], "publicKey": body["publicKey"]}}
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


def test_registers_with_a_signed_proof_and_writes_a_runner_config(tmp_path):
    app = App()
    try:
        binary = fake_binary(tmp_path)
        result = run(tmp_path, "--token", TOKEN, "--name", "alice-box", "--app",
                     app.url, "--binary", str(binary))
    finally:
        app.close()
    assert result.returncode == 0, result.stderr
    assert TOKEN not in result.stdout + result.stderr
    assert [path for path, _ in app.bodies] == ["/api/runners/register-token"]
    body = app.bodies[0][1]
    assert body["token"] == TOKEN and body["name"] == "alice-box"
    root = tmp_path / "root"
    key = root / "var/lib/forkmesh-runner/identity/ed25519.pem"
    assert stat.S_IMODE(key.stat().st_mode) == 0o600
    config_path = root / "etc/forkmesh-runner/runner.json"
    assert stat.S_IMODE(config_path.stat().st_mode) == 0o600
    config = json.loads(config_path.read_text())
    assert config["role"] == "runner"
    assert config["identityKey"] == "/var/lib/forkmesh-runner/identity/ed25519.pem"
    assert config["gatewayConfig"] == "/var/lib/forkmesh-runner/gateway/config.json"
    assert config["runner"]["allowedOwners"] == ["alice"]
    assert config["runner"]["appUrl"] == app.url
    assert config["runner"]["workRoot"] == "/var/lib/forkmesh-runner/work"
    assert config["listen"] != "127.0.0.1:8791"
    identity = json.loads((root / "var/lib/forkmesh-runner/gateway/config.json").read_text())
    assert identity == {"schemaVersion": 1, "node": {
        "name": "alice-box", "publicKey": body["publicKey"]}, "repositories": []}
    unit = (root / "etc/systemd/system/forkmesh-runner.service").read_text()
    assert "--config /etc/forkmesh-runner/runner.json" in unit
    assert "NoNewPrivileges=true" in unit and "IPAddressDeny=169.254.0.0/16" in unit
    nft = (root / "etc/forkmesh-runner/jobs.nft").read_text()
    assert 'meta skuid "forkmesh-job"' in nft and "192.168.0.0/16" in nft
    assert "flush ruleset" not in nft
    assert TOKEN not in json.dumps(config) and TOKEN not in unit
    assert (root / "usr/local/bin/forkmesh-mirror-node").is_file()


def test_a_second_run_keeps_the_key_and_a_refusal_is_reported(tmp_path):
    app = App()
    try:
        binary = fake_binary(tmp_path)
        run(tmp_path, "--token", TOKEN, "--name", "alice-box", "--app", app.url,
            "--binary", str(binary))
        first = app.bodies[-1][1]["publicKey"]
        again = run(tmp_path, "--token", TOKEN, "--name", "alice-box", "--app",
                    app.url, "--binary", str(binary))
        assert again.returncode == 0
        assert app.bodies[-1][1]["publicKey"] == first
        assert "Reusing the runner key" in again.stdout
        app.status = 401
        refused = run(tmp_path, "--token", TOKEN, "--name", "alice-box", "--app",
                      app.url, "--binary", str(binary))
    finally:
        app.close()
    assert refused.returncode == 1
    assert "HTTP 401" in refused.stderr and "unknown, used or expired" in refused.stderr


def test_the_script_deletes_nothing_recursively_and_never_flushes_the_firewall():
    text = SCRIPT.read_text()
    assert "rm -rf" not in text and "rm -r " not in text
    assert "flush ruleset" not in text
    assert "--uninstall" not in text and "--purge" not in text
    assert text.rstrip().endswith("}")
