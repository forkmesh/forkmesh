from __future__ import annotations

import asyncio
import base64
import json
import os
import stat
import sys
from pathlib import Path

HOST = Path(__file__).resolve().parents[1]
if str(HOST) not in sys.path:
    sys.path.insert(0, str(HOST))

from forkmesh_host import ssh_helper  # noqa: E402

BLOB = base64.b64encode(b"\x00\x00\x00\x0bssh-ed25519" + b"k" * 36).decode()
TOKEN = "fmrt_" + "c" * 64
PASSWORD = "p4ss-word-xyz"


class Writer:
    def __init__(self):
        self.messages = []

    def write(self, raw):
        self.messages.append(json.loads(raw))

    async def drain(self):
        pass


def fake_tool(tmp_path, name, body):
    path = tmp_path / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return str(path)


def request(**extra):
    base = {
        "op": "install", "address": "8.8.4.4", "port": 2222,
        "hostKey": {"type": "ssh-ed25519", "key": BLOB}, "user": "deploy",
        "password": PASSWORD, "privateKey": "", "passphrase": "",
        "installUrl": "https://forkmesh.com/install-runner.sh",
        "appUrl": "https://app.forkmesh.com", "token": TOKEN,
        "name": "alice-vps",
    }
    base.update(extra)
    return base


def test_validation_refuses_private_addresses_bad_keys_and_two_secrets():
    assert ssh_helper.validate_install(request()) == "8.8.4.4"
    for bad in ({"address": "10.0.0.1"}, {"address": "10.77.0.3"},
                {"address": "::ffff:8.8.8.8"}, {"port": 0},
                {"hostKey": {"type": "ssh-dss", "key": BLOB}},
                {"hostKey": {"type": "ssh-ed25519", "key": "!!"}},
                {"user": "bad user"}, {"token": "nope"}, {"name": "Bad"},
                {"installUrl": "http://x/install.sh"},
                {"appUrl": "https://x/path"},
                {"privateKey": "-----BEGIN"}, {"password": ""}):
        assert ssh_helper.validate_install(request(**bad)) == "", bad


def test_remote_command_quotes_and_elevates_only_when_needed():
    root = ssh_helper.remote_command(request(), "root", True)
    assert root.startswith("curl -fsSL 'https://forkmesh.com/install-runner.sh' | bash")
    assert "sudo" not in root
    with_password = ssh_helper.remote_command(request(), "deploy", True)
    assert with_password.startswith("sudo -S -p '' -v && curl")
    assert "| sudo -n bash -s -- --token '%s'" % TOKEN in with_password
    with_key = ssh_helper.remote_command(request(), "deploy", False)
    assert with_key.startswith("sudo -n true 2>/dev/null ||")
    assert PASSWORD not in with_password


def test_options_pin_the_host_key_and_disable_forwarding(tmp_path):
    options = " ".join(ssh_helper.ssh_options(str(tmp_path / "kh"),
                                              "ssh-rsa", 2222))
    for flag in ("StrictHostKeyChecking=yes", "GlobalKnownHostsFile=/dev/null",
                 "HostKeyAlgorithms=rsa-sha2-512,rsa-sha2-256",
                 "ForwardAgent=no", "ClearAllForwardings=yes",
                 "PermitLocalCommand=no", "-F /dev/null", "-p 2222"):
        assert flag in options
    assert ssh_helper.host_pattern("8.8.4.4", 22) == "8.8.4.4"
    assert ssh_helper.host_pattern("8.8.4.4", 2222) == "[8.8.4.4]:2222"


def test_install_passes_the_password_only_through_askpass_env_and_stdin(
        tmp_path, monkeypatch):
    record = tmp_path / "record.json"
    script = tmp_path / "fake_ssh.py"
    script.write_text("""
import json, os, sys
argv = sys.argv[1:]
known = [a.split("=", 1)[1] for a in argv if a.startswith("UserKnownHostsFile=")][0]
askpass = os.environ.get("SSH_ASKPASS", "")
json.dump({"argv": argv, "known": open(known).read(),
           "known_mode": oct(os.stat(known).st_mode & 0o777),
           "askpass_text": open(askpass).read(),
           "secret_env": os.environ.get("FORKMESH_SSH_SECRET", ""),
           "stdin": sys.stdin.read(),
           "env_keys": sorted(os.environ)}, open(%r, "w"))
print("installing " + %r)
print("ok")
""" % (str(record), TOKEN))
    ssh = fake_tool(tmp_path, "ssh", 'exec /usr/bin/python3 %s "$@"\n' % script)
    monkeypatch.setattr(ssh_helper, "SSH", ssh)
    writer = Writer()
    asyncio.run(ssh_helper.install(request(), writer))
    seen = json.loads(record.read_text())
    assert PASSWORD not in " ".join(seen["argv"])
    assert seen["secret_env"] == PASSWORD
    assert seen["stdin"] == PASSWORD + "\n"
    assert seen["known"] == "[8.8.4.4]:2222 ssh-ed25519 %s\n" % BLOB
    assert seen["known_mode"] == "0o600"
    assert "FORKMESH_SSH_SECRET" in seen["askpass_text"]
    assert PASSWORD not in seen["askpass_text"]
    assert "PubkeyAuthentication=no" in seen["argv"]
    assert seen["argv"][-3:-1] == ["deploy", "8.8.4.4"] or "8.8.4.4" in seen["argv"]
    assert "DATA_KEY" not in seen["env_keys"]
    lines = [m.get("out") for m in writer.messages if "out" in m]
    assert "installing [hidden]" in lines
    assert writer.messages[-1] == {"exit": 0}
    assert all(PASSWORD not in json.dumps(m) for m in writer.messages)
    assert not any(name.startswith("fm-ssh-") for name in os.listdir(
        os.path.dirname(os.path.dirname(
            [a.split("=", 1)[1] for a in seen["argv"]
             if a.startswith("UserKnownHostsFile=")][0]))))


def test_install_refuses_invalid_requests_without_running_ssh(tmp_path,
                                                             monkeypatch):
    monkeypatch.setattr(ssh_helper, "SSH", str(tmp_path / "missing"))
    writer = Writer()
    asyncio.run(ssh_helper.install(request(address="192.168.0.9"), writer))
    assert writer.messages == [{"exit": 2, "error": "The request was refused."}]


def test_exit_codes_are_explained(tmp_path, monkeypatch):
    monkeypatch.setattr(ssh_helper, "SSH", fake_tool(tmp_path, "ssh", "exit 255\n"))
    writer = Writer()
    asyncio.run(ssh_helper.install(request(), writer))
    assert writer.messages[-1]["exit"] == 255
    assert "could not sign in" in writer.messages[-1]["error"]


ADMIN_PASSWORD = "admin-pass-word-1"
MAIL_KEY = "SG.mailkey.secretvalue123"


def instance_request(**extra):
    base = {
        "op": "instance", "address": "8.8.4.4", "port": 2222,
        "hostKey": {"type": "ssh-ed25519", "key": BLOB}, "user": "deploy",
        "password": PASSWORD, "privateKey": "", "passphrase": "",
        "installUrl": "https://forkmesh.com/install-instance.sh",
        "config": {"projectName": "Acme Code", "adminPassword": ADMIN_PASSWORD,
                   "mailApiKey": MAIL_KEY, "domain": "code.example.com"},
    }
    base.update(extra)
    return base


def test_instance_validation_and_command():
    assert ssh_helper.validate_instance(instance_request()) == "8.8.4.4"
    for bad in ({"installUrl": "https://forkmesh.com/install.txt"},
                {"installUrl": "http://forkmesh.com/install-instance.sh"},
                {"config": "nope"}, {"config": {"x": "y" * 400000}},
                {"address": "10.0.0.4"}, {"password": "", "privateKey": ""}):
        assert ssh_helper.validate_instance(instance_request(**bad)) == "", bad
    logo = "data:image/png;base64," + "A" * ((200 * 1024 + 2) // 3 * 4)
    with_logo = instance_request(config=dict(instance_request()["config"], logo=logo))
    assert ssh_helper.validate_instance(with_logo) == "8.8.4.4"
    assert len(json.dumps(with_logo)) + 1 <= ssh_helper.MAX_REQUEST
    root = ssh_helper.instance_command(instance_request(), "root", True)
    assert root.startswith("t=$(mktemp) && curl -fsSL --proto =https")
    assert "'https://forkmesh.com/install-instance.sh'" in root
    assert 'bash "$t" --config-stdin' in root and "sudo" not in root
    with_password = ssh_helper.instance_command(instance_request(), "deploy", True)
    assert with_password.startswith("sudo -S -p '' -v && t=$(mktemp)")
    assert 'sudo -n bash "$t" --config-stdin' in with_password
    with_key = ssh_helper.instance_command(instance_request(), "deploy", False)
    assert with_key.startswith("sudo -n true 2>/dev/null ||")
    for command in (root, with_password, with_key):
        assert ADMIN_PASSWORD not in command and MAIL_KEY not in command


def test_instance_settings_travel_on_stdin_after_the_sudo_password(
        tmp_path, monkeypatch):
    record = tmp_path / "record.json"
    script = tmp_path / "fake_ssh.py"
    script.write_text("""
import json, sys
argv = sys.argv[1:]
data = sys.stdin.read()
json.dump({"argv": argv, "stdin": data}, open(%r, "w"))
print("FMI {\\"step\\":\\"system\\",\\"state\\":\\"running\\"}")
print("leaked " + %r + " and " + %r)
""" % (str(record), ADMIN_PASSWORD, MAIL_KEY))
    monkeypatch.setattr(ssh_helper, "SSH", fake_tool(
        tmp_path, "ssh", 'exec /usr/bin/python3 %s "$@"\n' % script))
    writer = Writer()
    request = instance_request()
    asyncio.run(ssh_helper.install(request, writer))
    assert request == {}
    seen = json.loads(record.read_text())
    joined = " ".join(seen["argv"])
    for secret in (PASSWORD, ADMIN_PASSWORD, MAIL_KEY):
        assert secret not in joined
    password_line, config_line, rest = seen["stdin"].split("\n", 2)
    assert password_line == PASSWORD and rest == ""
    config = json.loads(base64.b64decode(config_line))
    assert config["adminPassword"] == ADMIN_PASSWORD
    assert config["projectName"] == "Acme Code"
    lines = [m.get("out") for m in writer.messages if "out" in m]
    assert 'FMI {"step":"system","state":"running"}' in lines
    assert "leaked [hidden] and [hidden]" in lines
    assert writer.messages[-1] == {"exit": 0}


def test_instance_as_root_sends_only_the_settings(tmp_path, monkeypatch):
    record = tmp_path / "stdin.txt"
    monkeypatch.setattr(ssh_helper, "SSH", fake_tool(
        tmp_path, "ssh", "cat > %s\n" % record))
    writer = Writer()
    asyncio.run(ssh_helper.install(instance_request(user="root"), writer))
    lines = record.read_text().split("\n")
    assert len(lines) == 2 and lines[1] == ""
    assert json.loads(base64.b64decode(lines[0]))["mailApiKey"] == MAIL_KEY


def test_keyscan_parses_and_filters(tmp_path, monkeypatch):
    scan = fake_tool(tmp_path, "ssh-keyscan", """
echo '# comment'
echo "8.8.4.4 ssh-ed25519 %s"
echo "8.8.4.4 ssh-ed25519 %s"
echo "8.8.4.4 ssh-dss AAAAB3NzaC1kc3MAAACB"
""" % (BLOB, BLOB))
    monkeypatch.setattr(ssh_helper, "SSH_KEYSCAN", scan)
    writer = Writer()
    asyncio.run(ssh_helper.keyscan({"address": "8.8.4.4", "port": 22}, writer))
    assert writer.messages == [{"keys": [{"type": "ssh-ed25519", "key": BLOB}]}]
    writer = Writer()
    asyncio.run(ssh_helper.keyscan({"address": "127.0.0.1", "port": 22}, writer))
    assert writer.messages == [{"error": "refused address"}]


class ChunkedOutput:
    def __init__(self, chunks):
        self.chunks = list(chunks)

    async def read(self, _size):
        return self.chunks.pop(0) if self.chunks else b""


class ChunkedProcess:
    def __init__(self, chunks):
        self.stdout = ChunkedOutput(chunks)


def test_stream_splits_progress_lines_and_survives_long_output():
    writer = Writer()
    chunks = [b"Cloning\r50%\r100%\nSetting ", b"up git\r", b"\nsecret " + PASSWORD.encode() + b"\n",
              b"x" * (ssh_helper.MAX_PENDING + 10), b"tail"]
    asyncio.run(ssh_helper.stream(ChunkedProcess(chunks), writer, [PASSWORD]))
    lines = [message["out"] for message in writer.messages]
    assert lines[:4] == ["Cloning", "50%", "100%", "Setting up git"]
    assert "secret [hidden]" in lines
    assert any(len(line) == 2000 for line in lines)
    assert lines[-1] == "tail"
    assert all(PASSWORD not in line for line in lines)


def test_config_secrets_include_the_other_owners_passwords():
    config = {"adminPassword": "admin-secret-1", "mailApiKey": "",
              "owners": [{"username": "grace", "password": "grace-secret-2"},
                         {"username": "linus"}, "junk"]}
    assert ssh_helper.config_secrets(config) == ["admin-secret-1", "grace-secret-2"]
    assert ssh_helper.config_secrets({"owners": None}) == []
