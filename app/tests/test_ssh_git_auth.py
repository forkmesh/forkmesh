#!/usr/bin/env python3
"""SSH public-key management and node-side Git gateway coverage."""

import ast
import asyncio
import base64
import importlib.util
import json
import os
from pathlib import Path
import pwd
import shutil
import sqlite3
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest
from protection_fakes import UNPROTECTED  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
PROJECT_ROOT = ROOT.parent
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME_TEXT = (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
ENTRY_TEXT = (
    (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)
    + "\n\n\n"
    + (ENTRY.parent / "catalog_routes.py").read_text(encoding="utf-8")
)
APP_RUNTIME_TEXT = (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
SCHEMA_TEXT = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")


def _module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ssh_keys = _module(ROOT / "src" / "ssh_keys.py", "forkmesh_ssh_keys")
ssh_gateway = _module(PROJECT_ROOT / "tools" / "ssh_gateway.py", "forkmesh_ssh_gateway")


def _ssh_string(value):
    value = bytes(value)
    return len(value).to_bytes(4, "big") + value


def _key_line(key_type="ssh-ed25519", material=None, comment="laptop"):
    if material is None:
        material = b"\x42" * 32
    blob = _ssh_string(key_type.encode()) + _ssh_string(material)
    return "%s %s %s" % (key_type, base64.b64encode(blob).decode(), comment)


def _rsa_line(bits):
    modulus = b"\x00\x80" + b"\x00" * ((bits // 8) - 1)
    blob = _ssh_string(b"ssh-rsa") + _ssh_string(b"\x01\x00\x01") + _ssh_string(modulus)
    return "ssh-rsa " + base64.b64encode(blob).decode()


def _load_entry(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    assert found == set(names), "missing: %s" % sorted(set(names) - found)
    namespace = {"_branch_protection": UNPROTECTED, **(extra_globals or {})}
    exec(
        compile(
            ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[])),
            str(ENTRY),
            "exec",
        ),
        namespace,
    )
    return namespace


def _run(coro):
    return asyncio.run(coro)


def test_public_key_parser_normalizes_fingerprints_and_rejects_injection():
    parsed = ssh_keys.parse_public_key(_key_line())
    assert parsed["keyType"] == "ssh-ed25519"
    assert parsed["publicKey"].count(" ") == 1
    assert parsed["comment"] == "laptop"
    assert parsed["fingerprint"].startswith("SHA256:")
    assert (
        ssh_keys.parse_public_key(parsed["publicKey"])["fingerprint"]
        == parsed["fingerprint"]
    )

    for value, error in (
        ('command="sh" ' + _key_line(), "unsupported_key_type"),
        (_key_line() + "\n" + _key_line(comment="second"), "one_public_key_required"),
        ("ssh-dss AAAA", "unsupported_key_type"),
    ):
        with pytest.raises(ssh_keys.SshPublicKeyError, match=error):
            ssh_keys.parse_public_key(value)


def test_public_key_parser_requires_strong_rsa_and_matching_wire_type():
    assert ssh_keys.parse_public_key(_rsa_line(3072))["keyType"] == "ssh-rsa"
    with pytest.raises(ssh_keys.SshPublicKeyError, match="rsa_key_too_small"):
        ssh_keys.parse_public_key(_rsa_line(2048))
    mismatched = _key_line().replace("ssh-ed25519 ", "ssh-rsa ", 1)
    with pytest.raises(ssh_keys.SshPublicKeyError, match="key_type_mismatch"):
        ssh_keys.parse_public_key(mismatched)


def test_ssh_url_is_published_only_for_valid_gateway_configuration():
    assert (
        ssh_keys.ssh_repository_url("ssh.example.org", 22, "alice-node", "widget")
        == "ssh://git@ssh.example.org/alice-node/widget.git"
    )
    assert (
        ssh_keys.ssh_repository_url("2001:db8::1", 2222, "alice-node", "widget")
        == "ssh://git@[2001:db8::1]:2222/alice-node/widget.git"
    )
    for host in ("", "https://ssh.example.org", "git@ssh.example.org", "bad host"):
        assert ssh_keys.ssh_repository_url(host, 22, "alice-node", "widget") == ""


def test_gateway_token_and_repository_allowlist_fail_closed():
    assert ssh_keys.configured_gateway_token("t" * 32) == "t" * 32
    for token in (
        "t" * 31,
        " " + "t" * 32,
        "t" * 32 + "\n",
        "🔑" * 32,
        "t" * 31 + "/",
    ):
        assert ssh_keys.configured_gateway_token(token) == ""

    repositories = ssh_keys.parse_gateway_repository_allowlist(
        "alice-node/widget=read-write,alice-node/docs=ro"
    )
    assert repositories == {
        "alice-node/widget": "read-write",
        "alice-node/docs": "read-only",
    }
    assert (
        ssh_keys.gateway_repository_access(repositories, "alice-node", "Widget")
        == "read-write"
    )
    for invalid in (
        "",
        "disabled",
        "alice-node/widget",
        "alice-node/widget=admin",
        "alice-node/widget=read-only,alice-node/widget=read-write",
        "../widget=read-write",
    ):
        assert ssh_keys.parse_gateway_repository_allowlist(invalid) == {}


def test_gateway_node_hosts_are_validated_and_fail_closed():
    assert ssh_keys.parse_gateway_node_hosts(
        "mirror2=ssh.forkmesh.com,mirror3=198.51.100.3"
    ) == {
        "mirror2": "ssh.forkmesh.com",
        "mirror3": "198.51.100.3",
    }
    for invalid in (
        "mirror2",
        "mirror2=https://ssh.forkmesh.com",
        "../mirror=ssh.forkmesh.com",
        "mirror2=ssh-a.example,mirror2=ssh-b.example",
    ):
        assert ssh_keys.parse_gateway_node_hosts(invalid) == {}


def test_worker_publishes_url_only_for_explicit_gateway_repo():
    namespace = _load_entry(
        "_ssh_gateway_settings",
        "_ssh_repository_url",
        extra_globals={"ssh_auth": ssh_keys},
    )
    env = SimpleNamespace(
        SSH_GATEWAY_HOST="ssh.example.org",
        SSH_GATEWAY_PORT="22",
        SSH_GATEWAY_TOKEN="t" * 48,
        SSH_GATEWAY_REPOSITORIES="alice-node/widget=read-write",
    )
    assert (
        namespace["_ssh_repository_url"](env, "alice-node", "widget")
        == "ssh://git@ssh.example.org/alice-node/widget.git"
    )
    assert namespace["_ssh_repository_url"](env, "alice-node", "not-hosted") == ""
    env.SSH_GATEWAY_NODE_HOSTS = "alice-node=ssh-node.example.org"
    assert (
        namespace["_ssh_repository_url"](env, "alice-node", "widget")
        == "ssh://git@ssh-node.example.org/alice-node/widget.git"
    )
    env.SSH_GATEWAY_REPOSITORIES = ""
    assert namespace["_ssh_repository_url"](env, "alice-node", "widget") == ""


@pytest.mark.parametrize(
    "command",
    [
        "/opt/forkmesh/gateway --config /tmp/x",
        "/opt/forkmesh/gateway;sh",
        "/opt/forkmesh/$(id)",
        "/opt/forkmesh/`id`",
        "/opt/forkmesh/../gateway",
        "relative/gateway",
    ],
)
def test_forced_authorized_key_rejects_shell_command_injection(command):
    with pytest.raises(ValueError, match="invalid_gateway_executable"):
        ssh_keys.forced_authorized_key_line(_key_line(), "sk_" + "a" * 24, command)


def _non_root_broker_user():
    current = pwd.getpwuid(os.geteuid())
    if current.pw_uid != 0:
        return current.pw_name
    for candidate in pwd.getpwall():
        if candidate.pw_uid != 0:
            return candidate.pw_name
    raise RuntimeError("the test host has no non-root account")


def _gateway_config(tmp_path, **overrides):
    root = tmp_path / "repos"
    root.mkdir(parents=True)
    root.chmod(0o755)
    capacity = tmp_path / "capacity"
    capacity.mkdir(mode=0o700)
    true_program = str(Path(shutil.which("true") or "/usr/bin/true").resolve())
    config = {
        "schemaVersion": 1,
        "authorizationSocket": str(tmp_path / "authorize.sock"),
        "authorizationBrokerUser": _non_root_broker_user(),
        "gatewayExecutable": true_program,
        "refreshNotifier": true_program,
        "repositoryRoot": str(root),
        "limits": {
            **ssh_gateway.DEFAULT_LIMITS,
            "capacityDirectory": str(capacity),
        },
        "repositories": [
            {
                "owner": "alice-node",
                "name": "widget",
                "path": "alice-node/widget.git",
                "access": "read-write",
                "maxStorageBytes": 16 * 1024 * 1024 * 1024,
            }
        ],
    }
    config.update(overrides)
    path = tmp_path / "gateway.json"
    path.write_text(json.dumps(config), encoding="utf-8")
    path.chmod(0o644)
    return path, root


def _gateway_limits(tmp_path, **overrides):
    capacity = tmp_path / "runtime-capacity"
    capacity.mkdir(mode=0o700, exist_ok=True)
    values = {
        **ssh_gateway.DEFAULT_LIMITS,
        "capacityDirectory": str(capacity),
    }
    values.update(overrides)
    return ssh_gateway.GatewayLimits(values)


def test_gateway_config_rejects_symlink_writable_and_unknown_broker(
    tmp_path,
):
    path, _root = _gateway_config(tmp_path)
    assert ssh_gateway.Config(path).authorization_socket == (
        tmp_path / "authorize.sock"
    )

    path.chmod(0o666)
    with pytest.raises(ssh_gateway.GatewayError, match="invalid gateway configuration"):
        ssh_gateway.Config(path)
    path.chmod(0o644)

    symlink = tmp_path / "linked.json"
    symlink.symlink_to(path)
    with pytest.raises(ssh_gateway.GatewayError, match="invalid gateway configuration"):
        ssh_gateway.Config(symlink)

    value = json.loads(path.read_text(encoding="utf-8"))
    value["authorizationBrokerUser"] = "forkmesh-no-such-user"
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(
        ssh_gateway.GatewayError, match="authorization broker is unavailable"
    ):
        ssh_gateway.Config(path)


def test_gateway_runtime_rejects_unknown_fields_and_relative_token_path(
    tmp_path,
):
    path, _root = _gateway_config(tmp_path)
    value = json.loads(path.read_text(encoding="utf-8"))
    value["privateKey"] = "must-not-be-accepted"
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ssh_gateway.GatewayError, match="invalid gateway configuration"):
        ssh_gateway.Config(path)

    del value["privateKey"]
    value["authorizationSocket"] = "authorize.sock"
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ssh_gateway.GatewayError, match="invalid authorization socket"):
        ssh_gateway.Config(path)


def test_gateway_limits_reject_invalid_reservations_and_alias_quota_bypass(tmp_path):
    path, _root = _gateway_config(tmp_path)
    value = json.loads(path.read_text(encoding="utf-8"))
    value["limits"]["reservedReceiveSessions"] = value["limits"][
        "maxConcurrentSessions"
    ]
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(ssh_gateway.GatewayError, match="invalid gateway limits"):
        ssh_gateway.Config(path)

    value["limits"]["reservedReceiveSessions"] = 4
    value["repositories"].append(
        {
            "owner": "alias-node",
            "name": "widget",
            "path": "alice-node/widget.git",
            "access": "read-write",
            "maxStorageBytes": 17 * 1024 * 1024 * 1024,
        }
    )
    path.write_text(json.dumps(value), encoding="utf-8")
    with pytest.raises(
        ssh_gateway.GatewayError, match="invalid repository allowlist"
    ):
        ssh_gateway.Config(path)


def test_gateway_parses_only_exact_git_commands():
    assert ssh_gateway._parse_original_command(
        "git-receive-pack 'alice-node/widget.git'"
    ) == ("git-receive-pack", "alice-node", "widget")
    assert ssh_gateway._parse_original_command(
        "git-upload-pack '/alice-node/widget.git'"
    ) == ("git-upload-pack", "alice-node", "widget")
    for command in (
        "bash",
        "git-receive-pack '../../etc'",
        "git-receive-pack 'alice-node/widget.git' extra",
        "git-receive-pack 'alice-node/widget.git;id'",
    ):
        with pytest.raises(ssh_gateway.GatewayError):
            ssh_gateway._parse_original_command(command)
    with pytest.raises(ssh_gateway.GatewayError):
        ssh_gateway._parse_original_command("x" * 513)


def test_capacity_reserves_sessions_and_processes_for_authorized_pushes(tmp_path):
    limits = _gateway_limits(
        tmp_path,
        maxConcurrentSessions=3,
        reservedReceiveSessions=1,
        maxConcurrentProcesses=3,
        reservedReceiveProcesses=1,
    )
    for prefix in ("session", "process"):
        readers = [
            ssh_gateway._acquire_slot(
                limits.capacity_directory,
                prefix,
                maximum=3,
                reserved_receive=1,
                receive=False,
            )
            for _index in range(2)
        ]
        try:
            with pytest.raises(ssh_gateway.GatewayError, match="at capacity"):
                ssh_gateway._acquire_slot(
                    limits.capacity_directory,
                    prefix,
                    maximum=3,
                    reserved_receive=1,
                    receive=False,
                )
            writer = ssh_gateway._acquire_slot(
                limits.capacity_directory,
                prefix,
                maximum=3,
                reserved_receive=1,
                receive=True,
            )
            try:
                with pytest.raises(ssh_gateway.GatewayError, match="at capacity"):
                    ssh_gateway._acquire_slot(
                        limits.capacity_directory,
                        prefix,
                        maximum=3,
                        reserved_receive=1,
                        receive=True,
                    )
            finally:
                writer.close()
        finally:
            for reader in readers:
                reader.close()

    repository = tmp_path / "widget.git"
    first = ssh_gateway._acquire_repository_write_slot(
        limits.capacity_directory, repository
    )
    try:
        with pytest.raises(ssh_gateway.GatewayError, match="at capacity"):
            ssh_gateway._acquire_repository_write_slot(
                limits.capacity_directory, repository
            )
    finally:
        first.close()


def test_slow_git_service_kills_the_complete_process_group(tmp_path):
    marker = tmp_path / "escaped-child"
    child = (
        "import pathlib,signal,time;"
        "signal.signal(signal.SIGTERM, signal.SIG_IGN);"
        "time.sleep(0.8);"
        f"pathlib.Path({str(marker)!r}).write_text('alive')"
    )
    parent = (
        "import subprocess,sys,time;"
        f"subprocess.Popen([sys.executable,'-c',{child!r}]);"
        "time.sleep(30)"
    )
    with pytest.raises(ssh_gateway.GatewayError, match="deadline exceeded"):
        ssh_gateway._run_git_service(
            [sys.executable, "-c", parent],
            {
                "PATH": "/usr/bin:/bin",
                "LANG": "C",
                "LC_ALL": "C",
            },
            deadline_seconds=0.2,
            termination_grace_seconds=0.1,
        )
    time.sleep(0.9)
    assert not marker.exists()


def test_interrupted_receive_cleans_only_new_transaction_artifacts(tmp_path):
    repository = tmp_path / "widget.git"
    (repository / "objects" / "pack").mkdir(parents=True)
    (repository / "refs" / "heads").mkdir(parents=True)
    existing = repository / "refs" / "heads" / "existing.lock"
    existing.write_text("keep", encoding="ascii")
    before = ssh_gateway._receive_artifacts(repository, 5)

    quarantine = repository / "objects" / "incoming-test"
    quarantine.mkdir()
    (quarantine / "pack").write_bytes(b"partial")
    temporary_pack = repository / "objects" / "pack" / "tmp_partial"
    temporary_pack.write_bytes(b"partial")
    new_lock = repository / "refs" / "heads" / "main.lock"
    new_lock.write_text("partial", encoding="ascii")

    ssh_gateway._cleanup_interrupted_receive(repository, before, 5)
    assert existing.read_text(encoding="ascii") == "keep"
    assert not quarantine.exists()
    assert not temporary_pack.exists()
    assert not new_lock.exists()


def test_receive_max_input_size_rejects_an_oversized_pack(tmp_path):
    work = tmp_path / "work"
    bare = tmp_path / "repository.git"
    subprocess.run(
        ["git", "init", "-b", "main", str(work)],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["git", "-C", str(work), "config", "user.name", "Test"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(work), "config", "user.email", "test@example.invalid"],
        check=True,
    )
    (work / "payload.bin").write_bytes(os.urandom(256 * 1024))
    subprocess.run(["git", "-C", str(work), "add", "payload.bin"], check=True)
    subprocess.run(
        ["git", "-C", str(work), "commit", "-m", "oversized"],
        check=True,
        capture_output=True,
    )
    subprocess.run(
        ["git", "init", "--bare", str(bare)],
        check=True,
        capture_output=True,
    )
    wrapper = tmp_path / "bounded-receive-pack"
    wrapper.write_text(
        "#!/bin/sh\nexec git -c receive.maxInputSize=65536 "
        "-c receive.unpackLimit=0 receive-pack \"$@\"\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o700)
    pushed = subprocess.run(
        [
            "git",
            "-C",
            str(work),
            "push",
            "--receive-pack=" + str(wrapper),
            str(bare),
            "main",
        ],
        capture_output=True,
        check=False,
    )
    assert pushed.returncode != 0
    assert b"exceeds" in pushed.stderr.lower()


def test_gateway_rejects_receive_before_start_when_repository_is_over_quota(
    tmp_path,
    monkeypatch,
):
    root = tmp_path / "repos"
    repository = root / "alice-node" / "widget.git"
    repository.parent.mkdir(parents=True)
    subprocess.run(
        ["git", "init", "--bare", str(repository)],
        check=True,
        capture_output=True,
    )
    # Logical size is counted as well as allocated blocks, so a sparse file
    # cannot evade the application quota.
    oversized = repository / "objects" / "oversized"
    with oversized.open("wb") as stream:
        stream.truncate(17 * 1024 * 1024)
    limits = _gateway_limits(
        tmp_path,
        receiveMaxInputBytes=1024 * 1024,
        defaultRepositoryMaxBytes=16 * 1024 * 1024,
    )
    config = SimpleNamespace(
        repositories={
            ("alice-node", "widget"): (
                repository,
                True,
                16 * 1024 * 1024,
            )
        },
        repository_root=root,
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=limits,
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", "git-receive-pack 'alice-node/widget.git'"
    )
    monkeypatch.setattr(
        ssh_gateway,
        "_api",
        lambda _config, payload: {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": "alice-node/widget.git",
        },
    )
    monkeypatch.setattr(
        ssh_gateway,
        "_run_git_service",
        lambda *_args, **_kwargs: pytest.fail(
            "an over-quota repository must not start receive-pack"
        ),
    )
    with pytest.raises(ssh_gateway.GatewayError, match="storage quota exceeded"):
        ssh_gateway._serve(config, "sk_" + "a" * 24)


def test_gateway_runs_hardened_git_without_shell_after_authorization(
    monkeypatch,
    tmp_path,
):
    repo_path = Path("/srv/forkmesh/git/alice-node/widget.git")
    config = SimpleNamespace(
        repositories={("alice-node", "widget"): (repo_path, True)},
        repository_root=Path("/srv/forkmesh/git"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=_gateway_limits(tmp_path),
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", "git-receive-pack 'alice-node/widget.git'"
    )
    monkeypatch.setenv("GIT_PROTOCOL", "version=2")
    monkeypatch.setattr(
        ssh_gateway,
        "_api",
        lambda _config, payload: {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": (
                payload["owner"] + "/" + payload["repository"] + ".git"
            ),
        },
    )
    monkeypatch.setattr(ssh_gateway, "_bare_repository", lambda path: True)
    monkeypatch.setattr(Path, "resolve", lambda self, strict=False: self)
    monkeypatch.setattr(
        ssh_gateway, "_repository_size_bytes", lambda *_args, **_kwargs: 0
    )
    commands = []

    def fake_git(command, environment, **kwargs):
        commands.append((command, {**kwargs, "env": environment}))
        assert environment["GIT_TERMINAL_PROMPT"] == "0"
        assert environment["GIT_PROTOCOL"] == "version=2"
        return 0

    monkeypatch.setattr(ssh_gateway, "_run_git_service", fake_git)

    def fake_run(command, **kwargs):
        commands.append((command, kwargs))
        return SimpleNamespace(returncode=0)

    monkeypatch.setattr(ssh_gateway.subprocess, "run", fake_run)
    assert ssh_gateway._serve(config, "sk_" + "a" * 24) == 0
    git_command, git_options = commands[0]
    assert git_command[0] == "git"
    assert git_command[-2:] == ["receive-pack", str(repo_path)]
    for setting in (
        "core.alternateRefsCommand=/usr/bin/true",
        "core.hooksPath=/dev/null",
        "core.fsmonitor=",
        "credential.helper=",
        "protocol.ext.allow=never",
        "uploadpack.hideRefs=refs/forkmesh/",
        "uploadpack.allowTipSHA1InWant=false",
        "uploadpack.allowReachableSHA1InWant=false",
        "uploadpack.allowAnySHA1InWant=false",
        "uploadpack.allowFilter=true",
        "receive.hideRefs=refs/forkmesh/",
        "receive.unpackLimit=0",
        "pack.threads=2",
        "safe.directory=" + str(repo_path),
    ):
        assert setting in git_command
    assert any(
        setting.startswith("receive.maxInputSize=") for setting in git_command
    )
    assert git_options["deadline_seconds"] == 600
    assert git_options["termination_grace_seconds"] == 3
    assert not any(
        setting.startswith("uploadpack.packObjectsHook") for setting in git_command
    )
    assert "SSH_ORIGINAL_COMMAND" not in git_options["env"]
    assert commands[1][0] == [str(config.refresh_notifier)]
    assert commands[1][1]["stdin"] is subprocess.DEVNULL


def test_real_ssh_git_services_hide_and_protect_internal_merge_refs(tmp_path):
    work = tmp_path / "work"
    bare = tmp_path / "repository.git"
    subprocess.run(
        ["git", "init", "-b", "main", str(work)],
        check=True, capture_output=True)
    subprocess.run(
        ["git", "-C", str(work), "config", "user.name", "Test"],
        check=True)
    subprocess.run(
        ["git", "-C", str(work), "config", "user.email", "test@example.invalid"],
        check=True)
    (work / "README.md").write_text("public\n", encoding="utf-8")
    subprocess.run(
        ["git", "-C", str(work), "add", "README.md"], check=True)
    subprocess.run(
        ["git", "-C", str(work), "commit", "-m", "public"],
        check=True, capture_output=True)
    subprocess.run(
        ["git", "clone", "--bare", str(work), str(bare)],
        check=True, capture_output=True)
    commit = subprocess.run(
        ["git", "-C", str(work), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True).stdout.strip()
    subprocess.run(
        [
            "git", "--git-dir", str(bare), "update-ref",
            "refs/forkmesh/merge-completed/test/base", commit,
        ],
        check=True)

    advertisement = subprocess.run(
        [
            "git",
            "-c", "uploadpack.hideRefs=refs/forkmesh/",
            "-c", "uploadpack.allowTipSHA1InWant=false",
            "-c", "uploadpack.allowReachableSHA1InWant=false",
            "upload-pack", "--advertise-refs", str(bare),
        ],
        check=True,
        capture_output=True,
    ).stdout
    assert b"refs/forkmesh/" not in advertisement

    wrapper = tmp_path / "receive-pack-hidden-refs"
    wrapper.write_text(
        "#!/bin/sh\n"
        "exec git -c receive.hideRefs=refs/forkmesh/ receive-pack \"$@\"\n",
        encoding="utf-8",
    )
    wrapper.chmod(0o700)
    rejected = subprocess.run(
        [
            "git", "-C", str(work), "push",
            "--receive-pack=" + str(wrapper),
            str(bare), "HEAD:refs/forkmesh/merge-jobs/attacker",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert rejected.returncode != 0
    assert subprocess.run(
        [
            "git", "--git-dir", str(bare), "rev-parse", "--verify",
            "refs/forkmesh/merge-jobs/attacker",
        ],
        capture_output=True,
        check=False,
    ).returncode != 0


def test_gateway_allows_only_explicit_aliases_to_same_local_repository(
    monkeypatch,
    tmp_path,
):
    repo_path = Path("/srv/forkmesh/git/mirror2/forkmesh.git")
    config = SimpleNamespace(
        repositories={
            ("forkmesh", "forkmesh"): (repo_path, True),
            ("mirror2", "forkmesh"): (repo_path, True),
            ("mirror2", "other"): (Path("/srv/forkmesh/git/other.git"), True),
        },
        repository_root=Path("/srv/forkmesh/git"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
        limits=_gateway_limits(tmp_path),
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND",
        "git-receive-pack 'forkmesh/forkmesh.git'",
    )
    monkeypatch.setattr(ssh_gateway, "_bare_repository", lambda path: True)
    monkeypatch.setattr(Path, "resolve", lambda self, strict=False: self)
    monkeypatch.setattr(
        ssh_gateway, "_repository_size_bytes", lambda *_args, **_kwargs: 0
    )
    captured = []

    def fake_git(command, _environment, **_kwargs):
        captured.append(command)
        return 0

    monkeypatch.setattr(ssh_gateway, "_run_git_service", fake_git)
    monkeypatch.setattr(
        ssh_gateway.subprocess,
        "run",
        lambda *_args, **_kwargs: SimpleNamespace(returncode=0),
    )
    monkeypatch.setattr(
        ssh_gateway,
        "_api",
        lambda _config, payload: {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": "mirror2",
            "repository": "forkmesh",
            "operation": payload["operation"],
            "repositoryRelativePath": "mirror2/forkmesh.git",
        },
    )
    assert ssh_gateway._serve(config, "sk_" + "a" * 24) == 0
    assert captured[0][-1] == str(repo_path)

    config.repositories[("forkmesh", "forkmesh")] = (
        Path("/srv/forkmesh/git/other.git"),
        True,
    )
    with pytest.raises(ssh_gateway.GatewayError, match="not available"):
        ssh_gateway._serve(config, "sk_" + "a" * 24)


def test_gateway_refuses_receive_pack_for_local_read_only_repo(monkeypatch):
    config = SimpleNamespace(
        repositories={("alice-node", "widget"): (Path("/safe/widget.git"), False)},
        repository_root=Path("/safe"),
        refresh_notifier=Path("/opt/forkmesh/ssh-refresh-notify"),
    )
    monkeypatch.setenv(
        "SSH_ORIGINAL_COMMAND", "git-receive-pack 'alice-node/widget.git'"
    )
    monkeypatch.setattr(
        ssh_gateway,
        "_api",
        lambda _config, payload: {
            "authorized": True,
            "keyId": payload["keyId"],
            "owner": payload["owner"],
            "repository": payload["repository"],
            "operation": payload["operation"],
            "repositoryRelativePath": (
                payload["owner"] + "/" + payload["repository"] + ".git"
            ),
        },
    )
    with pytest.raises(ssh_gateway.GatewayError, match="read-only"):
        ssh_gateway._serve(config, "sk_" + "a" * 24)


def test_gateway_prints_worker_allowlist_from_same_local_configuration(
    tmp_path, capsys
):
    path, _root = _gateway_config(tmp_path)
    config = ssh_gateway.Config(path)
    assert ssh_gateway._worker_allowlist(config) == 0
    assert capsys.readouterr().out.strip() == "alice-node/widget=read-write"


def test_schema_and_migration_define_revocable_encrypted_key_records():
    migration = ROOT / "migrations" / "0067_account_ssh_keys.sql"
    assert migration.exists()
    sql = migration.read_text(encoding="utf-8")
    for source in (SCHEMA_TEXT, sql):
        assert "CREATE TABLE IF NOT EXISTS account_ssh_keys" in source
        assert "key_bi TEXT NOT NULL UNIQUE" in source
        assert "data TEXT NOT NULL" in source
        assert "revoked_at INTEGER NOT NULL DEFAULT 0" in source
    connection = sqlite3.connect(":memory:")
    try:
        connection.executescript(sql)
        connection.execute(
            "INSERT INTO account_ssh_keys "
            "(key_id,account_bi,key_bi,key_type,fingerprint,data,created_at) "
            "VALUES (?,?,?,?,?,?,?)",
            ("sk_one", "account", "key", "ssh-ed25519", "SHA256:x", "encrypted", 1),
        )
        with pytest.raises(sqlite3.IntegrityError):
            connection.execute(
                "INSERT INTO account_ssh_keys "
                "(key_id,account_bi,key_bi,key_type,fingerprint,data,created_at) "
                "VALUES (?,?,?,?,?,?,?)",
                ("sk_two", "other", "key", "ssh-ed25519", "SHA256:x", "encrypted", 2),
            )
    finally:
        connection.close()


def _response(payload, status=200, **kwargs):
    return {"payload": payload, "status": status, **kwargs}


class _Request:
    def __init__(self, payload, method="POST"):
        self.payload = payload
        self.method = method


def _authorization_handler(
    *,
    owns=False,
    org_write=False,
    gateway_access="read-write",
    authoritative_private=True,
    visibility="private",
):
    audits = []

    async def ensure_schema(_env):
        return None

    async def body(request):
        return request.payload, ""

    async def gateway_record(_env, key_id="", public_key=""):
        return {"key_id": key_id}, {"account": "alice"}

    async def d1_first(_env, sql, *params):
        assert "FROM repositories" in sql
        return {
            "is_private": 1 if authoritative_private else 0,
            "data": "encrypted",
        }

    async def audit(
        _env, actor, action, target_type="", target="", outcome="success", details=None
    ):
        audits.append((actor, action, outcome, details or {}))

    async def d1_run(*_args):
        return None

    namespace = _load_entry(
        "ssh_gateway_authorize_handler",
        extra_globals={
            "ensure_schema": ensure_schema,
            "method_name": lambda request: "POST",
            "_ssh_gateway_token_ok": lambda env, request: True,
            "_ssh_json_body": body,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 64,
            "safe_segment": lambda value: (
                str(value)
                if str(value)
                .replace("-", "")
                .replace("_", "")
                .replace(".", "")
                .isalnum()
                else ""
            ),
            "_ssh_key_gateway_record": gateway_record,
            "_org_repo_node": lambda *args: _async_value(""),
            "_ssh_gateway_repository_access": (lambda *args: gateway_access),
            "_ssh_gateway_bridge": lambda _env: False,
            "blind_index": lambda *args: _async_value("repo-bi"),
            "d1_first": d1_first,
            "decrypt_row": lambda *args: _async_value(
                {
                    "owner": "alice-node",
                    "name": "widget",
                    "visibility": visibility,
                }
            ),
            "_catalog_record_matches_identity": (
                lambda record, owner, repo: (
                    record["owner"] == owner and record["name"] == repo
                )
            ),
            "_account_owns_node": lambda *args: _async_value(owns),
            "_org_write_allowed": lambda *args: _async_value(org_write),
            "_repo_shared_with": lambda *args: _async_value(False),
            "_ssh_org_read_allowed": lambda *args: _async_value(False),
            "_audit_sensitive_action": audit,
            "d1_run": d1_run,
            "Date": SimpleNamespace(now=lambda: 1000),
            "json_response": _response,
            "re": __import__("re"),
        },
    )
    return namespace["ssh_gateway_authorize_handler"], audits


def test_gateway_rejects_bad_bearer_before_schema_body_or_database_work():
    calls = []

    async def forbidden(*_args, **_kwargs):
        calls.append("forbidden")
        raise AssertionError("unauthorized request reached protected work")

    namespace = _load_entry(
        "ssh_gateway_authorize_handler",
        extra_globals={
            "method_name": lambda _request: "POST",
            "_ssh_gateway_token_ok": lambda _env, _request: False,
            "ensure_schema": forbidden,
            "_ssh_json_body": forbidden,
            "json_response": _response,
        },
    )
    response = _run(
        namespace["ssh_gateway_authorize_handler"](SimpleNamespace(), _Request({}))
    )
    assert response["status"] == 401
    assert response["payload"] == {"error": "unauthorized"}
    assert calls == []


async def _async_value(value):
    return value


def _key_management_handler(
    method, *, existing=None, account_kind="user", listed_rows=None
):
    audits = []
    writes = []
    key_line = _key_line()

    async def session(*_args):
        return "bi:alice", {"name": "alice", "kind": account_kind, "status": "active"}

    async def d1_first(_env, sql, *params):
        if "WHERE key_bi=?" in sql:
            return existing
        if "COUNT(*)" in sql:
            return {"n": 0}
        if "WHERE key_id=? AND account_bi=?" in sql:
            return {"key_id": params[0]} if existing is not False else None
        raise AssertionError(sql)

    async def d1_run(_env, sql, *params):
        writes.append((sql, params))

    async def audit(
        _env, actor, action, target_type="", target="", outcome="success", details=None
    ):
        audits.append((actor, action, target_type, target, outcome, details or {}))

    namespace = _load_entry(
        "_ssh_key_public_projection",
        "ssh_keys_handler",
        extra_globals={
            "ensure_schema": lambda env: _async_value(None),
            "method_name": lambda request: request.method,
            "_ssh_json_body": lambda request: _async_value((request.payload, "")),
            "_account_session_record": session,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 64,
            "_account_kind": lambda record: record.get("kind"),
            "json_response": _response,
            "d1_all": lambda *args: _async_value(listed_rows or []),
            "decrypt_row": lambda *args: _async_value(
                {
                    "label": "Work laptop",
                    "publicKey": key_line,
                }
            ),
            "ssh_auth": ssh_keys,
            "blind_index": lambda *args: _async_value("bi:key"),
            "d1_first": d1_first,
            "_b64url_encode": lambda value: "a" * 24,
            "_random_bytes": lambda length: b"x" * length,
            "Date": SimpleNamespace(now=lambda: 1000),
            "encrypt_row": lambda env, data: _async_value("encrypted"),
            "d1_run": d1_run,
            "_audit_sensitive_action": audit,
            "re": __import__("re"),
            "_ssh_gateway_settings": lambda env: {
                "configured": False,
                "host": "",
                "port": 0,
            },
        },
    )
    request = _Request({"publicKey": key_line, "label": "Laptop"}, method=method)
    return namespace["ssh_keys_handler"], request, audits, writes


def test_account_key_registration_and_revocation_are_session_gated_and_audited():
    handler, request, audits, writes = _key_management_handler("POST")
    response = _run(handler(None, request))
    assert response["status"] == 201
    assert response["payload"]["privateKeysStored"] is False
    assert response["payload"]["key"]["fingerprint"].startswith("SHA256:")
    assert any("INSERT INTO account_ssh_keys" in sql for sql, _ in writes)
    assert audits[-1][1:5] == (
        "ssh_key.register",
        "ssh_public_key",
        "sk_" + "a" * 24,
        "success",
    )

    handler, request, audits, writes = _key_management_handler("DELETE", existing=True)
    response = _run(handler(None, request, "sk_" + "a" * 24))
    assert response["status"] == 200
    assert response["payload"]["revoked"] is True
    assert any("SET revoked_at=?" in sql for sql, _ in writes)
    assert audits[-1][1:5] == (
        "ssh_key.revoke",
        "ssh_public_key",
        "sk_" + "a" * 24,
        "success",
    )


def test_account_key_list_is_owner_only_and_never_returns_public_key_material():
    row = {
        "key_id": "sk_" + "a" * 24,
        "key_type": "ssh-ed25519",
        "fingerprint": "SHA256:example",
        "data": "encrypted",
        "created_at": 100,
        "last_used_at": 200,
    }
    handler, request, _audits, _writes = _key_management_handler(
        "GET", listed_rows=[row]
    )
    response = _run(handler(None, request))
    assert response["status"] == 200
    assert response["payload"]["privateKeysStored"] is False
    assert response["payload"]["keys"] == [
        {
            "id": row["key_id"],
            "label": "Work laptop",
            "keyType": "ssh-ed25519",
            "fingerprint": "SHA256:example",
            "createdAt": 100,
            "lastUsedAt": 200,
        }
    ]
    assert "publicKey" not in json.dumps(response["payload"])

    handler, request, _audits, _writes = _key_management_handler(
        "GET", account_kind="node"
    )
    response = _run(handler(None, request))
    assert response["status"] == 401
    assert response["payload"] == {"error": "invalid_session"}


def test_duplicate_key_registration_does_not_disclose_its_owner_or_state():
    handler, request, _audits, writes = _key_management_handler(
        "POST", existing={"account_bi": "bi:someone", "revoked_at": 100}
    )
    response = _run(handler(None, request))
    assert response["status"] == 409
    assert response["payload"] == {"error": "ssh_key_already_registered"}
    assert writes == []


def test_worker_gateway_maps_key_to_existing_owner_and_team_write_permissions():
    payload = {
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        "requestId": "r" * 24,
        "owner": "alice-node",
        "repository": "widget",
        "operation": "git-receive-pack",
    }
    handler, audits = _authorization_handler(owns=True)
    response = _run(handler(None, _Request(payload)))
    assert response["status"] == 200
    assert response["payload"]["authorized"] is True
    assert "principal" not in response["payload"]
    assert response["payload"]["keyId"] == payload["keyId"]
    assert response["payload"]["requestId"] == payload["requestId"]
    assert response["payload"]["repositoryRelativePath"] == "alice-node/widget.git"
    assert audits[-1][2] == "success"

    handler, audits = _authorization_handler(org_write=True)
    response = _run(handler(None, _Request(payload)))
    assert response["status"] == 200
    assert response["payload"]["authorized"] is True

    handler, audits = _authorization_handler()
    response = _run(handler(None, _Request(payload)))
    assert response["status"] == 403
    assert response["payload"] == {"authorized": False}
    assert audits[-1][2] == "denied"


def test_worker_gateway_public_read_requires_both_public_indicators():
    payload = {
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        "requestId": "r" * 24,
        "owner": "alice-node",
        "repository": "widget",
        "operation": "git-upload-pack",
    }
    handler, _ = _authorization_handler(
        authoritative_private=False, visibility="public"
    )
    assert _run(handler(None, _Request(payload)))["status"] == 200

    # Either disagreement fails closed and requires an owner/share/org ACL.
    for private_flag, visibility in (
        (True, "public"),
        (False, "private"),
    ):
        handler, _ = _authorization_handler(
            authoritative_private=private_flag, visibility=visibility
        )
        response = _run(handler(None, _Request(payload)))
        assert response["status"] == 403
        assert response["payload"] == {"authorized": False}


def test_worker_gateway_requires_repo_allowlist_and_write_mode():
    payload = {
        "action": "authorize",
        "keyId": "sk_" + "a" * 24,
        "requestId": "r" * 24,
        "owner": "alice-node",
        "repository": "widget",
        "operation": "git-receive-pack",
    }
    for access in ("", "read-only"):
        handler, audits = _authorization_handler(owns=True, gateway_access=access)
        response = _run(handler(None, _Request(payload)))
        assert response["status"] == 403
        assert response["payload"] == {"authorized": False}
        assert audits[-1][2] == "denied"


def test_renamed_account_is_resolved_from_current_account_index():
    line = _key_line(comment="")
    row = {
        "key_id": "sk_" + "a" * 24,
        "account_bi": "bi:new-name",
        "data": "encrypted",
    }

    async def d1_first(_env, sql, *params):
        assert "WHERE key_id=?" in sql
        return row

    namespace = _load_entry(
        "_ssh_key_gateway_record",
        extra_globals={
            "d1_first": d1_first,
            "decrypt_row": lambda *args: _async_value(
                {
                    "account": "old-name",
                    "publicKey": line,
                }
            ),
            "ssh_auth": ssh_keys,
            "_account_identity_rec_by_bi": lambda env, account_bi: _async_value(
                {
                    "name": "new-name",
                    "status": "active",
                    "kind": "user",
                }
            ),
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 64,
            "_account_kind": lambda record: record.get("kind"),
            "blind_index": lambda *args: _async_value("unused"),
            "hmac": __import__("hmac"),
        },
    )
    found, data = _run(namespace["_ssh_key_gateway_record"](None, key_id=row["key_id"]))
    assert found == row
    assert data["account"] == "new-name"
    # The production helper resolves the principal from row.account_bi, not
    # from the historical registration-time value, and overwrites the returned
    # authorization context with the current name.
    function_source = ast.get_source_segment(
        ENTRY_TEXT,
        next(
            node
            for node in ast.parse(ENTRY_TEXT).body
            if isinstance(node, ast.AsyncFunctionDef)
            and node.name == "_ssh_key_gateway_record"
        ),
    )
    assert "_account_identity_rec_by_bi" in function_source


def test_routes_frontend_and_deploy_contract_are_wired():
    assert '"/api/ssh/authorize"' in APP_RUNTIME_TEXT
    assert '"/api/accounts/ssh-keys"' in ENTRY_TEXT
    assert 'rec["sshUrl"]' in ENTRY_TEXT
    assert "node-side-forced-command" in ENTRY_TEXT
    settings = (
        ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
    ).read_text(encoding="utf-8")
    account_js = (ROOT / "public" / "dashboard" / "js" / "04-account.js").read_text(
        encoding="utf-8"
    )
    repo_js = (
        ROOT / "public" / "dashboard" / "js" / "08-repo-detail-network.js"
    ).read_text(encoding="utf-8")
    assert 'data-settings-section="ssh-keys"' in settings
    assert "data-ssh-public-key" in settings
    assert "/api/accounts/ssh-keys" in account_js
    assert "data-repo-ssh-url" in repo_js
    deploy = (PROJECT_ROOT / ".forkmesh" / "deploy.yml").read_text(encoding="utf-8")
    for name in (
        "SSH_GATEWAY_HOST",
        "SSH_GATEWAY_PORT",
        "SSH_GATEWAY_TOKEN",
        "SSH_GATEWAY_REPOSITORIES",
    ):
        assert "${{ vars.%s }}" % name in deploy
