"""tools/next_git_host.py: the private Git mirror for Codemagic."""

import subprocess
from pathlib import Path

import pytest

import next_common as common
import next_git_host as tool
import render
from fakes import FakeSsh

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"


def fleet(tmp_path):
    return render.load_fleet(FLEET, state_path=tmp_path / "none.json")


def test_snippet_requires_basic_auth_for_every_git_path():
    text = tool.snippet("$2a$14$" + "a" * 53)
    lines = [line.strip() for line in text.splitlines()]
    assert lines[lines.index("handle /git/* {") + 1] == "basic_auth {"
    assert "fmnext-ci $2a$14$" + "a" * 53 in lines
    assert "root * /var/lib/forkmesh-next/git" in lines
    assert "file_server" in lines and "browse" not in text
    with pytest.raises(common.ToolError):
        tool.snippet("plaintext")
    with pytest.raises(common.ToolError):
        tool.snippet("$2a$14$ has spaces")


def test_the_render_glob_matches_the_snippet_path():
    import fnmatch
    assert fnmatch.fnmatch(Path(tool.SNIPPET_PATH).name, render.GIT_PRIVATE_GLOB)
    assert tool.GIT_ROOT == render.GIT_PRIVATE_ROOT
    # setup's temporary file never matches the glob.
    assert not fnmatch.fnmatch(".git-private.abc123", render.GIT_PRIVATE_GLOB)


def test_setup_generates_the_password_once_and_sends_it_on_stdin_only(tmp_path, sandbox):
    (sandbox / "secrets.env").write_text("OTHER=keep\n")
    ssh = FakeSsh()
    runner = common.Runner(execute=True)
    tool.setup(fleet(tmp_path), ssh, runner)
    values = common.read_env_file(sandbox / "secrets.env")
    password = values["GIT_CI_PASSWORD"]
    assert values["OTHER"] == "keep" and len(password) == 43
    assert (sandbox / "secrets.env").stat().st_mode & 0o777 == 0o600
    assert len(ssh.runs) == 3
    for _host, script, stdin in ssh.runs:
        assert stdin == (password + "\n").encode()
        assert password not in script
        assert "caddy hash-password" in script and "systemctl reload caddy" in script
        assert "install -d -o root -g caddy -m 0750 /var/lib/forkmesh-next/git" in script
        assert "chmod 0640" in script
    # A second run keeps the password.
    tool.setup(fleet(tmp_path), FakeSsh(), runner)
    assert common.read_env_file(sandbox / "secrets.env")["GIT_CI_PASSWORD"] == password


def test_setup_script_writes_the_hash_not_the_password(tmp_path):
    # Run the snippet-writing part of the script locally with a fake caddy.
    script = tool.setup_script()
    body = script.split("tmp=$(mktemp", 1)[1].split("trap - EXIT", 1)[0]
    fake = tmp_path / "bin"
    fake.mkdir()
    (fake / "caddy").write_text("#!/bin/sh\nread x; echo '$2a$14$abc./DEF'\n")
    (fake / "caddy").chmod(0o755)
    (fake / "chown").write_text("#!/bin/sh\nexit 0\n")
    (fake / "chown").chmod(0o755)
    target = tmp_path / "git-private.caddy"
    local = ("IFS= read -r pw\nhash=$(printf '%s\\n' \"$pw\" | caddy hash-password)\n"
             "unset pw\ntmp=$(mktemp " + body).replace(
        "/etc/forkmesh-next/caddy/.git-private.XXXXXX", str(tmp_path / ".git-private.XXXXXX")
    ).replace(tool.SNIPPET_PATH, str(target))
    completed = subprocess.run(["bash", "-euo", "pipefail", "-c", local],
                               input=b"s3cret-password\n", capture_output=True,
                               env={"PATH": "%s:/usr/bin:/bin" % fake}, check=False)
    assert completed.returncode == 0, completed.stderr
    text = target.read_text()
    assert "fmnext-ci $2a$14$abc./DEF" in text and "s3cret" not in text
    assert "@HASH@" not in text


def test_dry_run_changes_nothing(tmp_path, sandbox):
    ssh = FakeSsh()
    runner = common.Runner(execute=False)
    tool.setup(fleet(tmp_path), ssh, runner)
    calls = []
    tool.push(fleet(tmp_path), ssh, runner, repo=REPO,
              run=lambda *a, **k: calls.append(a) or subprocess.CompletedProcess(
                  a, 0, "f" * 40 + "\n", ""))
    assert ssh.runs == []
    assert not (sandbox / "secrets.env").exists()
    assert all(c[0][3] == "rev-parse" for c in calls)


def test_push_uses_the_fleet_key_and_checks_what_is_served(tmp_path):
    main = "a" * 40
    ssh = FakeSsh(answers=[(lambda h, s: "update-server-info" in s, (main + "\n").encode())])
    ssh.key, ssh.known_hosts = Path("/k/key"), Path("/k/known_hosts")
    calls = []

    def run(argv, **kwargs):
        calls.append((argv, kwargs))
        return subprocess.CompletedProcess(argv, 0, main + "\n", "")
    tool.push(fleet(tmp_path), ssh, common.Runner(execute=True), repo=REPO, run=run)
    pushes = [c for c in calls if "push" in c[0]]
    assert len(pushes) == 3
    argv, kwargs = pushes[0]
    assert argv[-3].startswith("root@") and argv[-3].endswith(
        ":/var/lib/forkmesh-next/git/forkmesh-next.git")
    assert argv[-2:] == ["refs/heads/main:refs/heads/main", "refs/tags/*:refs/tags/*"]
    command = kwargs["env"]["GIT_SSH_COMMAND"]
    assert "-i /k/key" in command and "UserKnownHostsFile=/k/known_hosts" in command
    assert "IdentitiesOnly=yes" in command and "BatchMode=yes" in command
    assert len(ssh.runs) == 3
    assert all("chmod 0640" in script for _, script, _ in ssh.runs)


def test_push_fails_when_the_node_serves_another_commit(tmp_path):
    ssh = FakeSsh(answers=[(lambda h, s: True, b"b" * 40 + b"\n")])
    ssh.key, ssh.known_hosts = Path("/k/key"), Path("/k/known_hosts")
    run = lambda argv, **k: subprocess.CompletedProcess(argv, 0, "a" * 40 + "\n", "")  # noqa: E731
    with pytest.raises(common.ToolError):
        tool.push(fleet(tmp_path), ssh, common.Runner(execute=True), repo=REPO, run=run)


def test_url_never_has_the_password(tmp_path, sandbox, capsys):
    (sandbox / "secrets.env").write_text("GIT_CI_PASSWORD=do-not-print-me\n")
    assert tool.main(["--fleet", str(FLEET), "url"]) == 0
    out = capsys.readouterr().out.strip()
    assert out == "https://fmnext-ci@app.next.forkmesh.com/git/forkmesh-next.git"
    assert tool.main(["--fleet", str(FLEET), "url", "--node", "n2"]) == 0
    assert capsys.readouterr().out.strip() == \
        "https://fmnext-ci@n2.next.forkmesh.com/git/forkmesh-next.git"
