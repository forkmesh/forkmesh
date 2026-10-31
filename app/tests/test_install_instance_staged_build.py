import json
import shlex
import subprocess
from pathlib import Path

from test_install_instance_script import TEXT, function, sandbox_env

FUNCTIONS = (
    "files_digest", "lines", "pip_lines", "logo_file", "save_logo", "source_git",
    "source_retry", "fetch_origin_ref", "clone_source",
    "follow_source_url", "fetch_source", "checkout_target", "requirements_digest",
    "stage_venv", "stage_source", "activate_source", "rollback_source",
    "build_stamp", "release_version", "write_build_info", "write_daemon_state",
    "build_app", "on_exit", "record_daemon_build", "install_daemon_binary",
    "daemon_rebuild_notice",
)

FAKE_PYTHON = """#!/bin/bash
printf '%s\\n' "$*" >> "$FAKE_LOG"
case "$*" in
  *"forkmesh_host build"*)
    if [ -n "${FAIL_BUILD:-}" ]; then echo "build broke"; exit 3; fi
    mkdir -p ../dist && git rev-parse HEAD > ../dist/built-from ;;
esac
exit 0
"""


def git(cwd, *args, env):
    return subprocess.run(["git", "-C", str(cwd), *args], env=env, check=True,
                          capture_output=True, text=True).stdout.strip()


def commit(repo, env, name, text, server="v1"):
    (repo / "app" / "host").mkdir(parents=True, exist_ok=True)
    (repo / "server").mkdir(exist_ok=True)
    (repo / "desktop").mkdir(exist_ok=True)
    (repo / "desktop" / "CMakeLists.txt").write_text(
        "project(ForkMesh VERSION 1.4.2 LANGUAGES CXX)\n")
    (repo / "app" / "host" / "requirements.txt").write_text("cryptography\n")
    (repo / "server" / "main.go").write_text("package main // %s\n" % server)
    (repo / name).write_text(text)
    git(repo, "add", "-A", env=env)
    git(repo, "commit", "-qm", name, env=env)
    return git(repo, "rev-parse", "HEAD", env=env)


def setup(tmp_path):
    env = sandbox_env(tmp_path)
    env.pop("FORKMESH_INSTANCE_PARSE_ONLY", None)
    env.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_NOSYSTEM="1",
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.com",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.com",
               FAKE_LOG=str(tmp_path / "python.log"))
    upstream = tmp_path / "upstream"
    upstream.mkdir()
    git(upstream, "init", "-q", "-b", "main", env=env)
    first = commit(upstream, env, "a.txt", "a")
    opt = tmp_path / "opt"
    opt.mkdir()
    subprocess.run(["git", "clone", "-q", "--depth", "1", "file://%s" % upstream,
                    str(opt / "src")], env=env, check=True)
    venv = opt / "venv" / "bin"
    venv.mkdir(parents=True)
    (venv / "python").write_text(FAKE_PYTHON)
    (venv / "python").chmod(0o755)
    (opt / "src" / "app" / "dist").mkdir(parents=True)
    (opt / "src" / "app" / "dist" / "built-from").write_text(first + "\n")
    return env, upstream, opt, first


def harness(tmp_path, env, opt, upstream, body, extra=""):
    prelude = "\n".join([
        "set -euo pipefail",
        "say() { printf '%s\\n' \"$*\"; }",
        "warn() { printf 'Warning: %s\\n' \"$*\"; }",
        "progress() { :; }",
        "begin() { :; }",
        "die() { printf 'Error: %s\\n' \"$1\"; exit 1; }",
        "CURRENT_STEP=\"\" DETACHED=0 CONFIG_MODE=update",
        "OPT=%s" % shlex.quote(str(opt)),
        "SRC=\"$OPT/src\" VENV=\"$OPT/venv\"",
        "NEXT_SRC=\"$OPT/src.next\" PREV_SRC=\"$OPT/src.prev\"",
        "NEXT_VENV=\"$OPT/venv.next\" PREV_VENV=\"$OPT/venv.prev\"",
        "BUILD_SRC=\"$SRC\" BUILD_VENV=\"$VENV\" TARGET_COMMIT=\"\"",
        "SWAPPED=0 VENV_SWAPPED=0 APP_RESTARTED=0 SOURCE_COMMITTED=0 APP_CHANGED=0",
        "BUILD_STAMP=\"$OPT/build.stamp\" BUILD_INFO=\"$OPT/build-info.json\"",
        "DAEMON_BUILD=\"$OPT/daemon-build.json\" DAEMON=\"$OPT/daemon\"",
        "RELEASE_SOURCE=https://app.forkmesh.com/forkmesh/forkmesh",
        "ETC=\"$OPT/etc\" INSTANCE_INSTALLER_VERSION=2 EXCLUDED_PACKAGES=x",
        "CFG_projectName=Acme CFG_poweredBy=1 CFG_logoType= CFG_sourceRef=main",
        "CFG_sourceUrl=file://%s DOMAIN=code.example.com" % upstream,
        "systemctl() { printf 'systemctl %s\\n' \"$*\" >> \"$OPT/systemctl.log\"; }",
        "FORKMESH_SOURCE_RETRY_WAITS=0",
        extra,
        "trap on_exit EXIT",
    ])
    script = tmp_path / "harness.sh"
    script.write_text(prelude + "\n" + "".join(function(n) for n in FUNCTIONS) + body)
    return subprocess.run(["bash", str(script)], capture_output=True, text=True,
                          env=env, timeout=120)


def first_build(tmp_path, env, opt, upstream):
    result = harness(tmp_path, env, opt, upstream,
                     "fetch_source\nbuild_app\nactivate_source\n"
                     "SOURCE_COMMITTED=1\n")
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def test_a_failed_build_leaves_the_running_version_untouched(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    first_build(tmp_path, env, opt, upstream)
    running = json.loads((opt / "build-info.json").read_text())
    second = commit(upstream, env, "b.txt", "b")
    env["FAIL_BUILD"] = "1"
    result = harness(tmp_path, env, opt, upstream,
                     "fetch_source\nbuild_app\nactivate_source\n")
    env.pop("FAIL_BUILD")
    assert result.returncode != 0
    assert git(opt / "src", "rev-parse", "HEAD", env=env) == first
    assert (opt / "src" / "app" / "dist" / "built-from").read_text().strip() == first
    assert json.loads((opt / "build-info.json").read_text()) == running
    assert git(opt / "src.next", "rev-parse", "HEAD", env=env) == second
    assert not (opt / "src.prev").exists()


def test_a_good_build_switches_and_a_later_failure_rolls_back(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    first_build(tmp_path, env, opt, upstream)
    second = commit(upstream, env, "b.txt", "b")
    result = harness(tmp_path, env, opt, upstream,
                     "fetch_source\nbuild_app\nactivate_source\n"
                     "APP_RESTARTED=1\nfalse\n")
    assert result.returncode != 0
    assert "Kept the version that was running" in result.stdout
    assert "systemctl restart forkmesh-instance.service" in (
        opt / "systemctl.log").read_text()
    assert git(opt / "src", "rev-parse", "HEAD", env=env) == first
    assert json.loads((opt / "build-info.json").read_text())["commit"] == first
    assert git(opt / "src.next", "rev-parse", "HEAD", env=env) == second

    first_build(tmp_path, env, opt, upstream)
    assert git(opt / "src", "rev-parse", "HEAD", env=env) == second
    assert (opt / "src" / "app" / "dist" / "built-from").read_text().strip() == second
    assert git(opt / "src.prev", "rev-parse", "HEAD", env=env) == first
    assert json.loads((opt / "build-info.json").read_text())["commit"] == second
    (opt / "src.prev" / "recycled-marker").write_text("x")
    third = commit(upstream, env, "c.txt", "c")
    first_build(tmp_path, env, opt, upstream)
    assert (opt / "src" / "recycled-marker").exists()
    assert git(opt / "src", "rev-parse", "HEAD", env=env) == third
    assert sorted(p.name for p in opt.iterdir() if p.name.startswith("src")) == [
        "src", "src.prev"]


def test_changed_requirements_build_a_venv_beside_the_running_one(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    first_build(tmp_path, env, opt, upstream)
    (opt / "venv" / "running-marker").write_text("x")
    (upstream / "app" / "host" / "requirements.txt").write_text("cryptography\nhttpx\n")
    git(upstream, "commit", "-qam", "deps", env=env)
    first_build(tmp_path, env, opt, upstream)
    log = (tmp_path / "python.log").read_text()
    assert "-m pip install" in log
    assert not (opt / "venv" / "running-marker").exists()
    assert (opt / "venv.prev" / "running-marker").exists()
    digest = (opt / "venv" / ".forkmesh-requirements").read_text().strip()
    assert len(digest) == 64


def test_the_source_follows_the_configured_url(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    other = tmp_path / "other"
    subprocess.run(["git", "clone", "-q", str(upstream), str(other)], env=env, check=True)
    moved = commit(other, env, "own.txt", "own")
    result = harness(tmp_path, env, opt, other, "fetch_source\nbuild_app\nactivate_source\n"
                     "SOURCE_COMMITTED=1\n")
    assert result.returncode == 0, result.stdout + result.stderr
    assert git(opt / "src", "remote", "get-url", "origin", env=env) == "file://%s" % other
    assert git(opt / "src", "rev-parse", "HEAD", env=env) == moved
    assert "The source now follows" in result.stdout


def test_an_own_instance_url_is_fetched_through_the_local_web_server():
    body = function("source_git")
    assert '"https://$DOMAIN/"*) git -c "http.curloptResolve=$DOMAIN:443:127.0.0.1"' in body


def test_the_git_node_program_is_kept_and_flagged_when_server_code_moves(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    supplied = tmp_path / "node-binary"
    supplied.write_text("binary")
    env["FORKMESH_INSTANCE_DAEMON"] = str(supplied)
    first_build(tmp_path, env, opt, upstream)
    result = harness(tmp_path, env, opt, upstream, "install_daemon_binary\ndaemon_rebuild_notice\n")
    env.pop("FORKMESH_INSTANCE_DAEMON")
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads((opt / "daemon-build.json").read_text())["from"] == "supplied"
    assert json.loads((opt / "build-info.json").read_text())["daemon"]["state"] == "current"
    (opt / "daemon").write_text("kept")
    commit(upstream, env, "d.txt", "d", server="v2")
    result = harness(tmp_path, env, opt, upstream,
                     "fetch_source\nbuild_app\nactivate_source\nSOURCE_COMMITTED=1\n"
                     "curl() { echo fetched-a-release; return 1; }\n"
                     "install_daemon_binary\ndaemon_rebuild_notice\n")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "fetched-a-release" not in result.stdout
    assert (opt / "daemon").read_text() == "kept"
    assert "Kept the installed git node program" in result.stdout
    assert "server/ changed since the git node program was built" in result.stdout
    daemon = json.loads((opt / "build-info.json").read_text())["daemon"]
    assert daemon["state"] == "stale" and daemon["builtTree"] != daemon["sourceTree"]


def test_the_release_binary_is_only_fetched_for_the_release_source():
    body = function("install_daemon_binary")
    assert '[ "${CFG_sourceUrl%.git}" != "$RELEASE_SOURCE" ]' in body
    assert body.index("Kept the installed git node program") < body.index("curl -fsSL")
    order = TEXT.split("\ncheck_system\n", 1)[1]
    steps = ["build_app\n", "activate_source\n", "write_settings\n", "migrate_database\n",
             "start_app\n", "SOURCE_COMMITTED=\"1\""]
    found = [order.index(step) for step in steps]
    assert found == sorted(found)
    assert "rollback_source || true" in function("on_exit")
    assert 'APP_RESTARTED="1"\n  systemctl restart forkmesh-instance.service' in function("start_app")


def test_a_new_source_is_recorded_even_when_nothing_is_rebuilt(tmp_path):
    env, upstream, opt, first = setup(tmp_path)
    first_build(tmp_path, env, opt, upstream)
    built = json.loads((opt / "build-info.json").read_text())
    own = tmp_path / "own"
    subprocess.run(["git", "clone", "-q", "--bare", str(upstream), str(own)],
                   env=env, check=True)
    result = harness(tmp_path, env, opt, own,
                     "fetch_source\nbuild_app\nactivate_source\nSOURCE_COMMITTED=1\n")
    assert result.returncode == 0, result.stdout + result.stderr
    assert "already built" in result.stdout
    info = json.loads((opt / "build-info.json").read_text())
    assert info["source"] == "file://%s" % own and info["ref"] == "main"
    assert info["builtAt"] == built["builtAt"] and info["commit"] == first
