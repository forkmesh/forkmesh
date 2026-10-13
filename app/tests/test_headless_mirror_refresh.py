"""Security and lifecycle tests for the headless mirror refresh orchestrator."""

from __future__ import annotations

import base64
from dataclasses import replace
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
from types import SimpleNamespace

from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)
import pytest


ROOT = Path(__file__).resolve().parents[2]
REFRESH_PATH = ROOT / "tools" / "headless_mirror_refresh.py"
HELPER_PATH = ROOT / "tools" / "headless_mirror_identity.py"
GATEWAY_PATH = ROOT / "tools" / "mirror_gateway.py"
TUNNEL_PATH = ROOT / "tools" / "cloudflare_tunnel_bootstrap.py"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


refresh_tool = _load("headless_mirror_refresh_test", REFRESH_PATH)
identity_helper = _load("headless_mirror_refresh_identity_test", HELPER_PATH)
tunnel_helper = _load("headless_mirror_refresh_tunnel_test", TUNNEL_PATH)


def _run(command: list[str], cwd: Path | None = None) -> str:
    return subprocess.run(
        command,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
        env={
            "PATH": os.environ.get("PATH", ""),
            "LANG": "C",
            "LC_ALL": "C",
        },
    ).stdout.strip()


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).decode("ascii").rstrip("=")


def _write_actions_lease(
    installation,
    *,
    now_ms: int,
    state: str,
    node: str = "mirror-two",
    updated_at: int | None = None,
    expires_at: int | None = None,
) -> Path:
    path = installation["gateway_state"] / "actions-state.json"
    path.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": "forkmesh.mirror-actions-state",
                "node": node,
                "state": state,
                "updatedAt": now_ms if updated_at is None else updated_at,
                "expiresAt": (
                    now_ms + 10 * 60 * 1000
                    if expires_at is None
                    else expires_at
                ),
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def test_safe_environment_allows_only_owner_private_tmpdir(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    private = tmp_path / "mirror-runtime"
    private.mkdir(mode=0o700)
    monkeypatch.setenv("TMPDIR", str(private))
    assert refresh_tool._safe_environment()["TMPDIR"] == str(private)

    private.chmod(0o755)
    assert "TMPDIR" not in refresh_tool._safe_environment()

    private.chmod(0o700)
    link = tmp_path / "runtime-link"
    link.symlink_to(private, target_is_directory=True)
    monkeypatch.setenv("TMPDIR", str(link))
    assert "TMPDIR" not in refresh_tool._safe_environment()

    monkeypatch.setenv("TMPDIR", "relative-runtime")
    assert "TMPDIR" not in refresh_tool._safe_environment()


@pytest.fixture()
def installation(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    tmp_path.chmod(0o700)
    programs = tmp_path / "programs"
    programs.mkdir(mode=0o700)
    recipient = "age1" + "q" * 58
    age_secret = "AGE-SECRET-KEY-1" + "Q" * 58
    keygen = programs / "age-keygen"
    keygen.write_text(
        f"""#!{sys.executable}
import pathlib
import sys
if "-o" in sys.argv:
    target = pathlib.Path(sys.argv[sys.argv.index("-o") + 1])
    target.write_text("# test identity\\n{age_secret}\\n")
    raise SystemExit(0)
if "-y" in sys.argv:
    print("{recipient}")
    raise SystemExit(0)
raise SystemExit(2)
""",
        encoding="utf-8",
    )
    keygen.chmod(0o755)
    age = programs / "age"
    age.write_text(
        f"""#!{sys.executable}
import pathlib
import sys
HEADER = b"age-encryption.org/v1\\n"
if "--encrypt" in sys.argv:
    target = pathlib.Path(sys.argv[sys.argv.index("--output") + 1])
    target.write_bytes(HEADER + sys.stdin.buffer.read())
    raise SystemExit(0)
if "--decrypt" in sys.argv:
    raw = pathlib.Path(sys.argv[-1]).read_bytes()
    if not raw.startswith(HEADER):
        raise SystemExit(2)
    sys.stdout.buffer.write(raw[len(HEADER):])
    raise SystemExit(0)
raise SystemExit(2)
""",
        encoding="utf-8",
    )
    age.chmod(0o755)
    installed_helper = programs / "headless_mirror_identity.py"
    installed_gateway = programs / "mirror_gateway.py"
    shutil.copyfile(HELPER_PATH, installed_helper)
    shutil.copyfile(GATEWAY_PATH, installed_gateway)
    installed_helper.chmod(0o644)
    installed_gateway.chmod(0o644)
    monkeypatch.setenv(
        "PATH", str(programs) + os.pathsep + os.environ.get("PATH", "")
    )

    work = tmp_path / "work"
    bare = tmp_path / "source.git"
    work.mkdir(mode=0o700)
    _run(["git", "init", "-b", "main"], work)
    _run(["git", "config", "user.name", "ForkMesh test"], work)
    _run(["git", "config", "user.email", "test@example.invalid"], work)
    (work / "README.md").write_text("headless refresh\n", encoding="utf-8")
    _run(["git", "add", "README.md"], work)
    _run(["git", "commit", "-m", "initial"], work)
    _run(["git", "init", "--bare", str(bare)])
    _run(["git", "remote", "add", "mirror", str(bare)], work)
    _run(["git", "push", "mirror", "main"], work)
    _run(["git", "--git-dir", str(bare), "symbolic-ref", "HEAD", "refs/heads/main"])

    state = tmp_path / "identity"
    archive = tmp_path / "archives"
    release_store = tmp_path / "releases"
    gateway_state = tmp_path / "gateway-state"
    archive.mkdir(mode=0o700)
    release_store.mkdir(mode=0o700)
    gateway_state.mkdir(mode=0o700)
    router_key = Ed25519PrivateKey.generate()
    from cryptography.hazmat.primitives import serialization

    router_public = _b64url(
        router_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    )
    public = identity_helper.initialize_identity(
        state,
        {
            "schemaVersion": 1,
            "type": "forkmesh.headless-mirror-identity-init",
            "nodeName": "mirror-two",
            "routerPublicKey": router_public,
            "allowedOrigins": ["https://mirror-two.example.invalid"],
        },
        age_keygen_program=str(keygen),
    )
    unsigned = tunnel_helper.unsigned_tunnel_manifest(
        hostname="mirror-two.example.invalid",
        node_name="mirror-two",
        public_key=public["nodePublicKey"],
        tunnel_id="11111111-2222-3333-4444-555555555555",
        generated_at="2026-07-23T20:00:00Z",
    )
    payload = identity_helper._canonical_json(unsigned)
    signed = identity_helper.sign_mirror_manifest(
        state,
        {
            "schemaVersion": 1,
            "type": "forkmesh.mirror-endpoint-signing-request",
            "algorithm": "Ed25519",
            "encoding": "base64url-no-padding",
            "canonicalization": "forkmesh-json-sort-v1",
            "publicKey": public["nodePublicKey"],
            "payloadBase64": _b64url(payload),
            "payloadSha256": hashlib.sha256(payload).hexdigest(),
        },
    )
    manifest = dict(unsigned)
    manifest["signature"] = {
        "algorithm": "Ed25519",
        "encoding": "base64url-no-padding",
        "canonicalization": "forkmesh-json-sort-v1",
        "payloadSha256": hashlib.sha256(payload).hexdigest(),
        "value": signed["signature"],
    }
    manifest_path = gateway_state / "forkmesh-mirror.json"
    manifest_path.write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    manifest_path.chmod(0o600)

    python_program = programs / "python"
    python_program.write_text(
        "#!/bin/sh\nexec " + shlex.quote(sys.executable) + " \"$@\"\n",
        encoding="utf-8",
    )
    python_program.chmod(0o755)
    git_program = Path(shutil.which("git") or "/usr/bin/git").resolve()
    config_value = {
        "schemaVersion": 1,
        "type": "forkmesh.headless-mirror-refresh",
        "sourceRepository": str(bare),
        "archiveDirectory": str(archive),
        "releaseStore": str(release_store),
        "gatewayConfigPath": str(gateway_state / "mirror-gateway.json"),
        "identityStateDirectory": str(state),
        "identityHelperPath": str(installed_helper),
        "mirrorGatewayPath": str(installed_gateway),
        "pythonProgram": str(python_program),
        "gitProgram": str(git_program),
        "manifestPath": str(manifest_path),
        "nodeOwner": "mirror-two",
        "repositoryName": "forkmesh",
        "ownerAliases": ["mirror-two", "forkmesh"],
        "workerOrigin": "https://worker.example.invalid",
        "publicOrigin": "https://mirror-two.example.invalid",
        "listen": {"host": "127.0.0.1", "port": 8790},
        "operations": [
            "git-info-refs",
            "git-upload-pack",
            "tree",
            "blob",
            "raw",
            "branches",
        ],
        "catalog": {
            "description": "A living code city",
            "branch": "main",
            "platform": "git",
        },
    }
    config_path = tmp_path / "refresh.json"
    config_path.write_text(
        json.dumps(config_value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    config_path.chmod(0o600)
    return {
        "config_path": config_path,
        "config_value": config_value,
        "config": refresh_tool.load_config(config_path),
        "work": work,
        "bare": bare,
        "archive": archive,
        "release_store": release_store,
        "gateway_state": gateway_state,
        "gateway_config": gateway_state / "mirror-gateway.json",
        "public": public,
        "age_secret": age_secret,
    }


def test_refresh_renders_one_plaintext_mirror_for_all_aliases_and_check_is_dry(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    result = refresh_tool.refresh(config)
    assert result == {
        "ok": True,
        "event": "refresh_complete",
        "aliasCount": 2,
    }
    gateway_path = installation["gateway_config"]
    assert stat.S_IMODE(gateway_path.stat().st_mode) == 0o600
    rendered = json.loads(gateway_path.read_text(encoding="utf-8"))
    assert "actionsSummaryPath" not in rendered
    repositories = rendered["repositories"]
    assert [item["owner"] for item in repositories] == [
        "mirror-two",
        "forkmesh",
    ]
    assert {item["name"] for item in repositories} == {"forkmesh"}
    assert {
        item["releaseStore"] for item in repositories
    } == {str(installation["release_store"])}
    assert {item["gitDir"] for item in repositories} == {
        str(installation["bare"])
    }
    assert all("encryptedArchive" not in item for item in repositories)
    refs = {
        item["integrity"]["expectedRefsSha256"] for item in repositories
    }
    assert len(refs) == 1
    assert not list(installation["archive"].glob("archive-*.age"))

    before_config = gateway_path.read_bytes()
    before_archives = {
        item.name: hashlib.sha256(item.read_bytes()).hexdigest()
        for item in installation["archive"].iterdir()
    }
    lock_open_modes = []
    real_open = refresh_tool.os.open

    def tracked_open(path, flags, *args, **kwargs):
        if Path(path).name == ".refresh.lock":
            lock_open_modes.append(flags & os.O_ACCMODE)
        return real_open(path, flags, *args, **kwargs)

    monkeypatch.setattr(refresh_tool.os, "open", tracked_open)
    checked = refresh_tool.check(config)
    assert checked == {
        "ok": True,
        "event": "check_complete",
        "aliasCount": 2,
        "networkRequests": 0,
    }
    assert gateway_path.read_bytes() == before_config
    assert {
        item.name: hashlib.sha256(item.read_bytes()).hexdigest()
        for item in installation["archive"].iterdir()
    } == before_archives
    assert lock_open_modes
    assert set(lock_open_modes) == {os.O_RDONLY}
    combined = json.dumps(result) + json.dumps(checked)
    assert "mirror-two" not in combined
    assert "forkmesh" not in combined
    assert str(installation["bare"]) not in combined
    assert installation["age_secret"] not in gateway_path.read_text()


def test_refresh_never_seals_and_prunes_legacy_public_archives(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    archive_directory = installation["archive"]
    legacy = archive_directory / ("archive-" + "a" * 64 + ".age")
    legacy.write_bytes(b"legacy public ciphertext")
    legacy.chmod(0o600)

    real_helper_call = refresh_tool._helper_call

    def forbid_seal(config, mode, request=None):
        if mode == "seal-repository":
            raise AssertionError("unchanged refs must not be resealed")
        return real_helper_call(config, mode, request)

    monkeypatch.setattr(refresh_tool, "_helper_call", forbid_seal)
    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    assert tuple(archive_directory.glob("archive-*.age")) == ()

    monkeypatch.setattr(refresh_tool, "_helper_call", real_helper_call)
    (installation["work"] / "README.md").write_text(
        "new bounded generation\n", encoding="utf-8"
    )
    _run(["git", "add", "README.md"], installation["work"])
    _run(["git", "commit", "-m", "bounded generation"], installation["work"])
    _run(["git", "push", "mirror", "main"], installation["work"])
    refresh_tool.refresh(config)
    assert tuple(archive_directory.glob("archive-*.age")) == ()


def test_hosted_repository_sidecar_adds_direct_integrity_pinned_repository(
    installation,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
):
    config = installation["config"]
    imports_root = tmp_path / "imports"
    repository = imports_root / config.node_owner / "codeberg-demo.git"
    repository.parent.mkdir(parents=True)
    _run(["git", "clone", "--bare", str(installation["work"]), str(repository)])
    monkeypatch.setattr(
        refresh_tool,
        "HOSTED_REPOSITORIES_ROOT",
        imports_root,
    )
    sidecar = (
        config.gateway_config_path.parent
        / refresh_tool.HOSTED_REPOSITORIES_FILE
    )
    sidecar.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": refresh_tool.HOSTED_REPOSITORIES_TYPE,
                "repositories": [
                    {
                        "owner": config.node_owner,
                        "name": "codeberg-demo",
                        "sourceRepository": str(repository),
                        "sourceUrl": "https://codeberg.org/example/codeberg-demo",
                        "importId": "ext_" + "a" * 24,
                        "description": "Fully hosted import",
                        "branch": "main",
                        "createdAt": 1785000000000,
                        "publishedStateHash": "",
                    }
                ],
            }
        ),
        encoding="utf-8",
    )
    sidecar.chmod(0o600)

    rendered = refresh_tool._hosted_gateway_repositories(config)
    assert len(rendered) == 1
    assert rendered[0]["owner"] == config.node_owner
    assert rendered[0]["name"] == "codeberg-demo"
    assert rendered[0]["gitDir"] == str(repository)
    assert rendered[0]["integrity"]["expectedRefsSha256"] == (
        refresh_tool._repository_refs_sha256(config, repository)
    )
    assert "merge-pull" not in rendered[0]["operations"]
    assert "release-blob" not in rendered[0]["operations"]


def _push_hosting_config(
    installation,
    writable: Path,
    *,
    owners=("mirror-two",),
    receive_max_input_bytes: int | None = 268435456,
    name: str = "push-refresh.json",
):
    """Load a refresh configuration that opts this node into push hosting."""
    configured = json.loads(json.dumps(installation["config_value"]))
    configured["writableRepositoryRoot"] = str(writable)
    configured["writableOwners"] = list(owners)
    if receive_max_input_bytes is not None:
        configured["receiveMaxInputBytes"] = receive_max_input_bytes
    path = installation["config_path"].with_name(name)
    path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return refresh_tool.load_config(path)


def _write_push_sidecar(config, entries, *, envelope=None):
    path = (
        config.gateway_config_path.parent / refresh_tool.HOSTED_PUSHES_FILE
    )
    value = {
        "schemaVersion": 1,
        "type": refresh_tool.HOSTED_PUSHES_TYPE,
        "repositories": entries,
    }
    if envelope is not None:
        value = envelope
    path.write_text(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    return path


def _push_entry(writable: Path, owner: str, name: str):
    return {
        "owner": owner,
        "name": name,
        "gitDir": str(writable / owner / (name + ".git")),
        "firstPushedAt": 1785000000000,
        "lastPushedAt": 1785000000000,
    }


def _create_pushed_repository(installation, writable: Path, owner, name):
    repository = writable / owner / (name + ".git")
    repository.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    _run(["git", "clone", "--bare", str(installation["work"]), str(repository)])
    # Match the gateway's owner-only repository creation even when the test
    # runner has a collaboration-friendly umask (for example 0002).
    repository.chmod(0o700)
    return repository


def _render(installation, config):
    return refresh_tool._render_gateway_config(
        config,
        refresh_tool._load_public_identity(config),
        refresh_tool.SealMetadata(
            ciphertext_sha256="a" * 64,
            ciphertext_bytes=100,
            key_reference="public-plaintext",
            expected_refs_sha256="b" * 64,
        ),
        installation["bare"],
    )


def test_push_sidecar_renders_receive_pack_for_exactly_the_allowed_repository(
    installation,
    tmp_path: Path,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    outside = tmp_path / "outside"
    outside.mkdir(mode=0o700)
    config = _push_hosting_config(installation, writable)
    _create_pushed_repository(installation, writable, "mirror-two", "phone")
    _create_pushed_repository(installation, writable, "intruder", "phone")
    _run(["git", "clone", "--bare", str(installation["work"]),
          str(outside / "escaped.git")])

    _write_push_sidecar(
        config,
        [
            _push_entry(writable, "mirror-two", "phone"),
            # Not in writableOwners even though the directory really exists.
            _push_entry(writable, "intruder", "phone"),
            # Correct identity, storage outside the writable root.
            {
                "owner": "mirror-two",
                "name": "escaped",
                "gitDir": str(outside / "escaped.git"),
                "firstPushedAt": 1,
                "lastPushedAt": 1,
            },
            # Name the shared repository regex rejects.
            _push_entry(writable, "mirror-two", "bad/name"),
            # Would displace the node's own configured mirror.
            _push_entry(writable, "mirror-two", "forkmesh"),
            # Entry carrying an unexpected field.
            dict(
                _push_entry(writable, "mirror-two", "phone"),
                extra="unexpected",
            ),
        ],
    )

    rendered = _render(installation, config)
    assert rendered["writableRepositoryRoot"] == str(writable)
    assert rendered["writableOwners"] == ["mirror-two"]
    assert rendered["limits"] == {"receiveMaxInputBytes": 268435456}
    pushed = [
        item
        for item in rendered["repositories"]
        if "git-receive-pack" in item["operations"]
    ]
    assert len(pushed) == 1
    assert (pushed[0]["owner"], pushed[0]["name"]) == ("mirror-two", "phone")
    assert pushed[0]["gitDir"] == str(writable / "mirror-two" / "phone.git")
    assert pushed[0]["enabled"] is True
    assert pushed[0]["visibility"] == "public"
    assert refresh_tool.SHA256_RE.fullmatch(
        pushed[0]["integrity"]["expectedRefsSha256"]
    )
    assert set(pushed[0]["operations"]).isdisjoint(
        refresh_tool.DERIVED_REPOSITORY_EXCLUDED_OPERATIONS
    )
    # Every other repository, including the node's own aliases, stays
    # read-only and keeps exactly one entry per identity.
    identities = [
        (item["owner"], item["name"]) for item in rendered["repositories"]
    ]
    assert len(identities) == len(set(identities))
    assert identities.count(("mirror-two", "forkmesh")) == 1
    assert all(
        "git-receive-pack" not in item["operations"]
        for item in rendered["repositories"]
        if (item["owner"], item["name"]) != ("mirror-two", "phone")
    )
    assert "git-receive-pack" not in refresh_tool.PUBLIC_OPERATIONS


def test_wildcard_writable_owners_renders_any_pushed_owner(
    installation,
    tmp_path: Path,
):
    """A node holding website-created repositories cannot name its owners.

    Those repositories (issue #676) belong to accounts with no node of their
    own, so ["*"] replaces the name list. Every other sidecar check still
    applies: the storage path must be exactly the one the gateway derives.
    """
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    outside = tmp_path / "outside"
    outside.mkdir(mode=0o700)
    config = _push_hosting_config(installation, writable, owners=("*",))
    _create_pushed_repository(installation, writable, "web-newcomer", "app")
    _run(["git", "clone", "--bare", str(installation["work"]),
          str(outside / "escaped.git")])

    _write_push_sidecar(
        config,
        [
            _push_entry(writable, "web-newcomer", "app"),
            {
                "owner": "web-newcomer",
                "name": "escaped",
                "gitDir": str(outside / "escaped.git"),
                "firstPushedAt": 1,
                "lastPushedAt": 1,
            },
        ],
    )

    rendered = _render(installation, config)
    assert rendered["writableOwners"] == ["*"]
    pushed = [
        item
        for item in rendered["repositories"]
        if "git-receive-pack" in item["operations"]
    ]
    assert [(item["owner"], item["name"]) for item in pushed] == [
        ("web-newcomer", "app")]


def test_wildcard_writable_owners_must_be_the_only_entry(
    installation,
    tmp_path: Path,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    with pytest.raises(refresh_tool.RefreshError, match="wildcard"):
        _push_hosting_config(
            installation, writable, owners=("*", "mirror-two"),
            name="mixed-wildcard.json")


def test_push_hosting_keys_are_absent_from_an_ordinary_read_only_node(
    installation,
):
    rendered = _render(installation, installation["config"])
    assert "writableRepositoryRoot" not in rendered
    assert "writableOwners" not in rendered
    assert "limits" not in rendered
    assert all(
        "git-receive-pack" not in item["operations"]
        for item in rendered["repositories"]
    )

    configured = json.loads(json.dumps(installation["config_value"]))
    configured["operations"].append("git-receive-pack")
    path = installation["config_path"].with_name("write-operations.json")
    path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    with pytest.raises(refresh_tool.RefreshError, match="public routing"):
        refresh_tool.load_config(path)


def test_push_hosting_configuration_fails_closed_on_unsafe_values(
    installation,
    tmp_path: Path,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)

    def loaded(name, **changes):
        configured = json.loads(json.dumps(installation["config_value"]))
        configured.update(changes)
        path = installation["config_path"].with_name(name)
        path.write_text(
            json.dumps(configured, sort_keys=True, separators=(",", ":"))
            + "\n",
            encoding="utf-8",
        )
        path.chmod(0o600)
        return refresh_tool.load_config(path)

    with pytest.raises(refresh_tool.RefreshError, match="writableOwners"):
        loaded("owners-without-root.json", writableOwners=["mirror-two"])
    with pytest.raises(
        refresh_tool.RefreshError, match="receiveMaxInputBytes"
    ):
        loaded("limit-without-root.json", receiveMaxInputBytes=268435456)
    with pytest.raises(refresh_tool.RefreshError, match="writableOwners"):
        loaded(
            "duplicate-owners.json",
            writableRepositoryRoot=str(writable),
            writableOwners=["mirror-two", "Mirror-Two"],
        )
    with pytest.raises(refresh_tool.RefreshError, match="writableOwners"):
        loaded(
            "invalid-owner.json",
            writableRepositoryRoot=str(writable),
            writableOwners=["NOT AN OWNER"],
        )
    with pytest.raises(
        refresh_tool.RefreshError, match="receiveMaxInputBytes"
    ):
        loaded(
            "tiny-limit.json",
            writableRepositoryRoot=str(writable),
            receiveMaxInputBytes=1024,
        )
    group_readable = tmp_path / "group-readable"
    group_readable.mkdir(mode=0o750)
    with pytest.raises(
        refresh_tool.RefreshError, match="writable repository root"
    ):
        loaded(
            "unsafe-root.json",
            writableRepositoryRoot=str(group_readable),
        )
    link = tmp_path / "writable-link"
    link.symlink_to(writable, target_is_directory=True)
    with pytest.raises(
        refresh_tool.RefreshError, match="writable repository root"
    ):
        loaded("linked-root.json", writableRepositoryRoot=str(link))


def test_hostile_push_sidecar_is_ignored_and_never_fails_the_refresh(
    installation,
    tmp_path: Path,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    config = _push_hosting_config(installation, writable)
    _create_pushed_repository(installation, writable, "mirror-two", "phone")
    sidecar = (
        config.gateway_config_path.parent / refresh_tool.HOSTED_PUSHES_FILE
    )
    valid = [_push_entry(writable, "mirror-two", "phone")]

    def pushed_names():
        return [
            item["name"]
            for item in refresh_tool._pushed_gateway_repositories(
                config, frozenset()
            )
        ]

    # Baseline: the honest sidecar renders.
    _write_push_sidecar(config, valid)
    assert pushed_names() == ["phone"]

    # Wrong protocol type.
    _write_push_sidecar(
        config,
        valid,
        envelope={
            "schemaVersion": 1,
            "type": "forkmesh.hosted-repositories",
            "repositories": valid,
        },
    )
    assert pushed_names() == []

    # Unknown envelope field.
    _write_push_sidecar(
        config,
        valid,
        envelope={
            "schemaVersion": 1,
            "type": refresh_tool.HOSTED_PUSHES_TYPE,
            "repositories": valid,
            "trustMe": True,
        },
    )
    assert pushed_names() == []

    # Too many entries.
    _write_push_sidecar(
        config,
        valid * (refresh_tool.MAX_HOSTED_PUSHES + 1),
    )
    assert pushed_names() == []

    # Not JSON at all.
    sidecar.write_text("{not json", encoding="utf-8")
    sidecar.chmod(0o600)
    assert pushed_names() == []

    # Group-readable state written by another process.
    _write_push_sidecar(config, valid)
    sidecar.chmod(0o640)
    assert pushed_names() == []
    sidecar.chmod(0o600)

    # Oversized.
    sidecar.write_text(
        " " * (refresh_tool.MAX_HOSTED_PUSHES_BYTES + 1), encoding="utf-8"
    )
    sidecar.chmod(0o600)
    assert pushed_names() == []

    # A symbolic link pointing at an otherwise valid sidecar.
    real = config.gateway_config_path.parent / "elsewhere-pushes.json"
    real.write_text(
        json.dumps(
            {
                "schemaVersion": 1,
                "type": refresh_tool.HOSTED_PUSHES_TYPE,
                "repositories": valid,
            }
        ),
        encoding="utf-8",
    )
    real.chmod(0o600)
    sidecar.unlink()
    sidecar.symlink_to(real)
    assert pushed_names() == []
    sidecar.unlink()

    # Missing entirely.
    assert pushed_names() == []

    # None of this may stop the node's own mirror from refreshing, and a
    # hostile sidecar must never leave a partially rendered generation: the
    # document is dropped whole, so the cycle is byte-identical to a cycle
    # with no sidecar at all and the active generation still validates.
    _write_push_sidecar(
        config,
        valid,
        envelope={
            "schemaVersion": 1,
            "type": "forkmesh.hosted-repositories",
            "repositories": valid,
        },
    )
    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    hostile = installation["gateway_config"].read_bytes()
    rendered = json.loads(hostile.decode("utf-8"))
    assert all(
        "git-receive-pack" not in item["operations"]
        for item in rendered["repositories"]
    )
    assert refresh_tool.check(config)["event"] == "check_complete"
    sidecar.unlink()
    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    assert installation["gateway_config"].read_bytes() == hostile
    assert not [
        leftover
        for leftover in installation["gateway_config"].parent.iterdir()
        if leftover.name.startswith(".")
        and "pushes" not in leftover.name
    ]


def test_push_created_repository_is_registered_idempotently(
    installation,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    config = _push_hosting_config(installation, writable)
    _create_pushed_repository(installation, writable, "mirror-two", "phone")
    _write_push_sidecar(config, [_push_entry(writable, "mirror-two", "phone")])

    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    first = installation["gateway_config"].read_bytes()
    rendered = json.loads(first.decode("utf-8"))
    pushed = [
        item
        for item in rendered["repositories"]
        if item["name"] == "phone"
    ]
    assert len(pushed) == 1
    assert "git-receive-pack" in pushed[0]["operations"]

    # A second cycle with no new push must be byte-identical, and the active
    # generation must validate against a freshly rendered configuration.
    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    assert installation["gateway_config"].read_bytes() == first
    assert refresh_tool.check(config)["event"] == "check_complete"

    posted = []

    def post_json(url, value):
        posted.append((url, value))
        if url.endswith("/api/mirrors/https"):
            return 200, {
                "ok": True,
                "node": config.node_owner,
                "baseUrl": config.public_origin,
                "health": "active",
            }
        return 200, {
            "ok": True,
            "repository": {
                "owner": config.node_owner,
                "name": config.repository_name,
                "stateHash": value.get("stateHash"),
            },
        }

    result = refresh_tool.register(config, post_json=post_json)
    assert result["event"] == "registration_complete"
    # The node registers only its own endpoint and its own catalog record.
    # A push-created repository's catalog record belongs to the publishing
    # device (mobile node contract 7); the node must never publish one.
    catalog_posts = [
        value for url, value in posted if url.endswith("/api/repositories")
    ]
    assert len(catalog_posts) == 1
    assert catalog_posts[0]["name"] == config.repository_name
    assert all(item.get("name") != "phone" for item in catalog_posts)


def test_pushed_repository_refs_may_move_without_tripping_the_pin(
    installation,
    tmp_path: Path,
):
    writable = tmp_path / "writable"
    writable.mkdir(mode=0o700)
    config = _push_hosting_config(installation, writable)
    repository = _create_pushed_repository(
        installation, writable, "mirror-two", "phone")
    _write_push_sidecar(config, [_push_entry(writable, "mirror-two", "phone")])
    assert refresh_tool.refresh(config)["event"] == "refresh_complete"
    active = installation["gateway_config"]
    pinned = json.loads(active.read_text(encoding="utf-8"))
    pin = {
        item["name"]: item["integrity"]["expectedRefsSha256"]
        for item in pinned["repositories"]
    }

    # A further authorized push moves the pushed repository's refs while the
    # signed configuration still names the previous generation.
    (installation["work"] / "PUSHED.md").write_text("pushed\n", encoding="utf-8")
    _run(["git", "add", "PUSHED.md"], installation["work"])
    _run(["git", "commit", "-m", "pushed generation"], installation["work"])
    _run(["git", "--git-dir", str(repository), "fetch", "--prune",
          str(installation["work"]), "+refs/heads/*:refs/heads/*"])
    assert (
        refresh_tool._repository_refs_sha256(config, repository)
        != pin["phone"]
    )

    # The gateway must still accept the unchanged configuration: writable
    # repositories are exempt from the immutable bootstrap pin.
    refresh_tool._invoke_gateway_check(config, active)

    # The node's own mirror keeps the strict pin: moving its refs behind the
    # signed configuration quarantines it and fails the same check.
    _run(["git", "push", "mirror", "main"], installation["work"])
    with pytest.raises(refresh_tool.RefreshError):
        refresh_tool._invoke_gateway_check(config, active)


def test_actions_status_operation_renders_only_fixed_adjacent_summary_path(
    installation,
):
    configured = json.loads(json.dumps(installation["config_value"]))
    configured["operations"].append("actions-status")
    path = installation["config_path"].with_name(
        "actions-status-refresh.json")
    path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    path.chmod(0o600)
    config = refresh_tool.load_config(path)
    rendered = refresh_tool._render_gateway_config(
        config,
        refresh_tool._load_public_identity(config),
        refresh_tool.SealMetadata(
            ciphertext_sha256="a" * 64,
            ciphertext_bytes=100,
            key_reference="forkmesh-headless-age:test",
            expected_refs_sha256="b" * 64,
        ),
        installation["archive"] / ("archive-" + "a" * 64 + ".age"),
    )
    expected = (
        installation["gateway_config"].parent / "actions-summary.json")
    assert rendered["actionsSummaryPath"] == str(expected)
    assert all(
        "actions-status" in repository["operations"]
        for repository in rendered["repositories"]
    )
    schema = json.loads(
        (ROOT / "www" / "docs" / "headless-mirror-refresh.schema.json")
        .read_text(encoding="utf-8"))
    operations = schema["properties"]["operations"]
    assert operations["maxItems"] == len(refresh_tool.PUBLIC_OPERATIONS)
    assert "actions-status" in operations["items"]["enum"]


def test_check_never_creates_a_missing_refresh_lock(installation):
    lock = installation["config"].identity_state_directory / ".refresh.lock"
    assert not lock.exists()
    with pytest.raises(refresh_tool.RefreshError, match="lock"):
        refresh_tool.check(installation["config"])
    assert not lock.exists()


def test_failed_staged_gateway_validation_preserves_last_good_pair(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    old_config = installation["gateway_config"].read_bytes()

    (installation["work"] / "README.md").write_text(
        "headless refresh changed\n", encoding="utf-8"
    )
    _run(["git", "add", "README.md"], installation["work"])
    _run(["git", "commit", "-m", "change"], installation["work"])
    _run(["git", "push", "mirror", "main"], installation["work"])

    def reject_staged(_config, _path):
        raise refresh_tool.RefreshError("gateway validation rejected the active mirror")

    monkeypatch.setattr(refresh_tool, "_invoke_gateway_check", reject_staged)
    with pytest.raises(refresh_tool.RefreshError, match="gateway validation"):
        refresh_tool.refresh(config)
    assert installation["gateway_config"].read_bytes() == old_config


def test_catalog_host_telemetry_config_is_explicit_and_boolean(installation):
    config = installation["config"]
    assert config.catalog.report_cpu is False
    assert config.catalog.report_memory is False
    assert config.catalog.report_disk is False

    configured = json.loads(json.dumps(installation["config_value"]))
    configured["catalog"].update({
        "reportCpu": True,
        "reportMemory": True,
        "reportDisk": True,
    })
    configured_path = installation["config_path"].parent / "telemetry-refresh.json"
    configured_path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    configured_path.chmod(0o600)
    loaded = refresh_tool.load_config(configured_path)
    assert loaded.catalog.report_cpu is True
    assert loaded.catalog.report_memory is True
    assert loaded.catalog.report_disk is True

    configured["catalog"]["reportCpu"] = "true"
    configured_path.write_text(json.dumps(configured), encoding="utf-8")
    with pytest.raises(refresh_tool.RefreshError, match="must be a boolean"):
        refresh_tool.load_config(configured_path)


def test_catalog_actions_capability_is_explicit_bounded_and_disabled_by_default(
    installation,
):
    config = installation["config"]
    assert config.catalog.actions_enabled is False

    configured = json.loads(json.dumps(installation["config_value"]))
    configured["catalog"]["actionsEnabled"] = True
    configured_path = installation["config_path"].parent / "actions-refresh.json"
    configured_path.write_text(
        json.dumps(configured, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    configured_path.chmod(0o600)
    loaded = refresh_tool.load_config(configured_path)
    assert loaded.catalog.actions_enabled is True

    invalid_values = ("true", 1, None, "running")
    for index, invalid in enumerate(invalid_values):
        candidate = json.loads(json.dumps(installation["config_value"]))
        candidate["catalog"]["actionsEnabled"] = invalid
        path = configured_path.with_name(f"actions-invalid-{index}.json")
        path.write_text(json.dumps(candidate), encoding="utf-8")
        path.chmod(0o600)
        with pytest.raises(refresh_tool.RefreshError, match="must be a boolean"):
            refresh_tool.load_config(path)

    for prohibited in (
        "actionsState",
        "actionsVariables",
        "actionsCommand",
        "actionsWorkingDirectory",
        "actionsLogs",
    ):
        candidate = json.loads(json.dumps(installation["config_value"]))
        candidate["catalog"][prohibited] = "must-not-publish"
        path = configured_path.with_name(f"{prohibited}.json")
        path.write_text(json.dumps(candidate), encoding="utf-8")
        path.chmod(0o600)
        with pytest.raises(
            refresh_tool.RefreshError,
            match="unknown or missing field",
        ):
            refresh_tool.load_config(path)

    metadata = refresh_tool.SealMetadata(
        ciphertext_sha256="a" * 64,
        ciphertext_bytes=123,
        key_reference="age:test",
        expected_refs_sha256="b" * 64,
    )
    assert refresh_tool._catalog_unsigned(
        config, metadata, 1784840000000
    )["actionsState"] == "disabled"
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, 1784840000000
    )["actionsState"] == "enabled"

    now_ms = 1784840000000
    lease = _write_actions_lease(
        installation, now_ms=now_ms, state="running")
    disabled_catalog = refresh_tool._catalog_unsigned(
        config, metadata, now_ms)
    assert (
        disabled_catalog["actionsEnabled"],
        disabled_catalog["actionsState"],
    ) == (False, "disabled")
    running_catalog = refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms)
    assert (
        running_catalog["actionsEnabled"],
        running_catalog["actionsState"],
    ) == (True, "running")

    _write_actions_lease(installation, now_ms=now_ms, state="disabled")
    live_disabled = refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms)
    assert (
        live_disabled["actionsEnabled"],
        live_disabled["actionsState"],
    ) == (False, "disabled")

    _write_actions_lease(
        installation,
        now_ms=now_ms,
        state="running",
        updated_at=now_ms - refresh_tool.MAX_ACTIONS_STATE_LEASE_MS - 1,
    )
    stale_fallback = refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms)
    assert (
        stale_fallback["actionsEnabled"],
        stale_fallback["actionsState"],
    ) == (True, "enabled")

    _write_actions_lease(
        installation,
        now_ms=now_ms,
        state="running",
        expires_at=now_ms + refresh_tool.MAX_ACTIONS_STATE_LEASE_MS + 1,
    )
    future_fallback = refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms)
    assert (
        future_fallback["actionsEnabled"],
        future_fallback["actionsState"],
    ) == (True, "enabled")

    _write_actions_lease(
        installation,
        now_ms=now_ms,
        state="running",
        updated_at=now_ms + 1000,
        expires_at=now_ms + 1000,
    )
    zero_interval_fallback = refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms)
    assert (
        zero_interval_fallback["actionsEnabled"],
        zero_interval_fallback["actionsState"],
    ) == (True, "enabled")

    _write_actions_lease(
        installation,
        now_ms=now_ms,
        state="running",
        node="another-node",
    )
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms
    )["actionsState"] == "enabled"
    _write_actions_lease(installation, now_ms=now_ms, state="running")
    installation["gateway_state"].chmod(0o750)
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms
    )["actionsState"] == "enabled"
    installation["gateway_state"].chmod(0o700)
    lease.chmod(0o640)
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms
    )["actionsState"] == "enabled"
    lease.unlink()
    lease_target = installation["gateway_state"] / "actions-state-target.json"
    lease_target.write_text(
        json.dumps({
            "schemaVersion": 1,
            "type": "forkmesh.mirror-actions-state",
            "node": "mirror-two",
            "state": "running",
            "updatedAt": now_ms,
            "expiresAt": now_ms + 10 * 60 * 1000,
        }),
        encoding="utf-8",
    )
    lease_target.chmod(0o600)
    lease.symlink_to(lease_target)
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms
    )["actionsState"] == "enabled"
    lease.unlink()
    os.link(lease_target, lease)
    assert refresh_tool._catalog_unsigned(
        loaded, metadata, now_ms
    )["actionsState"] == "enabled"


def test_actions_state_handoff_accepts_only_exact_root_group_lease():
    def metadata(
        *,
        owner=0,
        group=992,
        mode=0o640,
        links=1,
        size=100,
        kind=stat.S_IFREG,
    ):
        return SimpleNamespace(
            st_mode=kind | mode,
            st_uid=owner,
            st_gid=group,
            st_nlink=links,
            st_size=size,
        )

    fixed = refresh_tool.SYSTEM_ACTIONS_STATE_PATH
    assert refresh_tool._actions_state_metadata_allowed(
        fixed, metadata(), effective_uid=991, effective_gid=992)
    assert refresh_tool._actions_state_metadata_allowed(
        Path("/srv/custom/actions-state.json"),
        metadata(owner=991, group=991, mode=0o600),
        effective_uid=991,
        effective_gid=992,
    )
    for path, info in (
        (Path("/srv/custom/actions-state.json"), metadata()),
        (fixed, metadata(owner=991)),
        (fixed, metadata(group=993)),
        (fixed, metadata(mode=0o600)),
        (fixed, metadata(mode=0o660)),
        (fixed, metadata(links=2)),
        (fixed, metadata(size=0)),
        (fixed, metadata(size=refresh_tool.MAX_ACTIONS_STATE_BYTES + 1)),
        (fixed, metadata(kind=stat.S_IFLNK)),
    ):
        assert not refresh_tool._actions_state_metadata_allowed(
            path, info, effective_uid=991, effective_gid=992)
    assert not refresh_tool._actions_state_metadata_allowed(
        fixed, metadata(), effective_uid=0, effective_gid=992)


def test_configure_actions_atomically_preserves_config_and_renews_public_state(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    before = json.loads(
        installation["config_path"].read_text(encoding="utf-8"))
    installation["config_path"].chmod(0o400)
    published = []

    def capture_publish(updated, identity, metadata, *, post_json):
        published.append((updated, identity, metadata, post_json))
        return {"ok": True, "aliasCount": len(updated.owner_aliases)}

    monkeypatch.setattr(
        refresh_tool, "_publish_registration", capture_publish)
    request = {
        "schemaVersion": 1,
        "type": "forkmesh.mirror-actions-catalog-configuration",
        "actionsEnabled": True,
    }
    result = refresh_tool.configure_actions(config, request)

    after = json.loads(
        installation["config_path"].read_text(encoding="utf-8"))
    expected = json.loads(json.dumps(before))
    expected.setdefault("catalog", {})["actionsEnabled"] = True
    assert after == expected
    assert stat.S_IMODE(installation["config_path"].stat().st_mode) == 0o400
    assert result == {
        "ok": True,
        "event": "actions_configuration_complete",
        "actionsEnabled": True,
    }
    assert len(published) == 1
    updated = published[0][0]
    assert updated.catalog.actions_enabled is True
    metadata = published[0][2]
    catalog = refresh_tool._catalog_unsigned(
        updated, metadata, 1784840000000)
    assert (catalog["actionsEnabled"], catalog["actionsState"]) == (
        True,
        "enabled",
    )

    unchanged = installation["config_path"].read_bytes()
    for invalid in (
        {
            **request,
            "actionsEnabled": 1,
        },
        {
            **request,
            "secret": "must-not-be-accepted",
        },
        {
            "schemaVersion": 1,
            "type": "forkmesh.mirror-actions-catalog-configuration",
        },
    ):
        with pytest.raises(refresh_tool.RefreshError):
            refresh_tool.configure_actions(updated, invalid)
        assert installation["config_path"].read_bytes() == unchanged
    assert len(published) == 1


def test_linux_host_metric_parsers_are_bounded_and_fail_closed(installation):
    snapshots = iter([
        b"cpu  100 0 100 800 0 0 0 0\n",
        b"cpu  150 0 150 900 0 0 0 0\n",
    ])
    assert refresh_tool._sample_linux_cpu(
        read_metric=lambda _path: next(snapshots),
        sleeper=lambda _seconds: None,
    ) == 50
    assert refresh_tool._proc_cpu_snapshot(b"cpu malformed\n") is None

    assert refresh_tool._parse_linux_memory(
        b"MemTotal:       1000 kB\nMemAvailable:    250 kB\n"
    ) == (750 * 1024, 1000 * 1024)
    assert refresh_tool._parse_linux_memory(
        b"MemTotal:       1000 kB\n"
    ) is None

    disk = refresh_tool._sample_linux_disk(
        installation["bare"],
        statvfs=lambda _path: SimpleNamespace(
            f_frsize=4096,
            f_bsize=4096,
            f_blocks=1000,
            f_bavail=250,
        ),
    )
    assert disk == (750 * 4096, 1000 * 4096)


def test_host_sampling_calls_only_opted_in_metrics_and_omits_unavailable(
    installation,
):
    config = installation["config"]

    def forbidden(*_args):
        raise AssertionError("disabled sampler was called")

    assert refresh_tool._sample_host_telemetry(
        config,
        cpu_sampler=forbidden,
        memory_sampler=forbidden,
        disk_sampler=forbidden,
        platform="linux",
    ) == {}

    enabled = replace(
        config,
        catalog=replace(
            config.catalog,
            report_cpu=True,
            report_memory=True,
            report_disk=True,
        ),
    )
    sampled = refresh_tool._sample_host_telemetry(
        enabled,
        cpu_sampler=lambda: 177,
        memory_sampler=lambda: (900, 800),
        disk_sampler=lambda _path: None,
        platform="linux",
    )
    assert sampled == {
        "cpuPercent": 100,
        "memUsedBytes": 800,
        "memTotalBytes": 800,
    }
    assert refresh_tool._sample_host_telemetry(
        enabled,
        cpu_sampler=lambda: None,
        memory_sampler=lambda: (10, 0),
        disk_sampler=lambda _path: (-1, 100),
        platform="linux",
    ) == {}


def test_headless_catalog_samples_truthful_repository_statistics(installation):
    work = installation["work"]

    records = {
        ".forkmesh/issues/open/1/issue-1.json": {
            "status": "open",
            "events": [{"type": "open", "author": "alice"}],
        },
        ".forkmesh/issues/open/2/issue-2.json": {
            "status": "open",
            "events": [
                {"type": "open", "author": "bob"},
                {
                    "type": "delete",
                    "target": "self",
                    "author": "bob",
                },
            ],
        },
        ".forkmesh/issues/closed/3/issue-3.json": {
            "status": "closed",
            "events": [],
        },
        ".forkmesh/issues/4/issue-4.json": {
            "status": "open",
            "events": [],
        },
        ".forkmesh/discussions/5/discussion-5.json": {"title": "one"},
        ".forkmesh/discussions/9/discussion-9.json": {"title": "two"},
    }
    for relative, value in records.items():
        target = work / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(value), encoding="utf-8")
    _run(["git", "add", ".forkmesh"], work)
    _run(["git", "commit", "-m", "add collaboration metadata"], work)
    _run(["git", "push", "mirror", "main"], work)

    _run(["git", "switch", "-c", "forkmesh/pulls"], work)
    for number in (7, 11):
        target = work / ".forkmesh" / "pulls" / str(number) / "pull.md"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"# Pull {number}\n", encoding="utf-8")
    _run(["git", "add", ".forkmesh/pulls"], work)
    _run(["git", "commit", "-m", "add pull metadata"], work)
    _run(["git", "push", "mirror", "forkmesh/pulls"], work)
    _run(["git", "switch", "main"], work)
    _run(["git", "branch", "feature/reliable-counts"], work)
    _run(["git", "push", "mirror", "feature/reliable-counts"], work)

    for digest in ("a" * 64, "b" * 64):
        data = (
            installation["release_store"]
            / "sha256"
            / digest[:2]
            / digest
            / "data"
        )
        data.parent.mkdir(parents=True, exist_ok=True)
        data.write_bytes(digest.encode("ascii"))

    stats = refresh_tool._sample_repository_statistics(
        installation["config"]
    )
    activity_weeks = stats.pop("activityWeeks")
    merge_activity_weeks = stats.pop("mergeActivityWeeks")
    assert {
        key: value for key, value in stats.items() if key != "commitAt"
    } == {
        "commitCount": "2",
        "mergeCount": "0",
        "branchCount": "3",
        "issueCount": "2",
        "issueMaxNumber": "4",
        "pullCount": "2",
        "discussionCount": "2",
        "artifactCount": "2",
        # Who made the published head commit and what it says, so the Mirror
        # nodes view and the World cabinets can name it (adhoc #337).
        "commitSubject": "add collaboration metadata",
        "commitAuthorName": "ForkMesh test",
    }
    assert len(activity_weeks) == 52
    assert sum(activity_weeks) == 2
    assert len(merge_activity_weeks) == 52
    assert sum(merge_activity_weeks) == 0
    assert int(stats["commitAt"]) > 0
    assert "worktreeCount" not in stats
    assert "clonesServed" not in stats
    assert "websiteServed" not in stats


def test_headless_catalog_omits_unreadable_or_unavailable_statistics(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = replace(installation["config"], release_store=None)
    monkeypatch.setattr(
        refresh_tool,
        "_source_blob_batch",
        lambda *_args, **_kwargs: None,
    )
    issue = installation["work"] / ".forkmesh/issues/open/1/issue-1.json"
    issue.parent.mkdir(parents=True, exist_ok=True)
    issue.write_text('{"status":"open","events":[]}', encoding="utf-8")
    _run(["git", "add", ".forkmesh"], installation["work"])
    _run(["git", "commit", "-m", "add one issue"], installation["work"])
    _run(["git", "push", "mirror", "main"], installation["work"])

    stats = refresh_tool._sample_repository_statistics(config)
    assert "issueCount" not in stats
    # The tree itself still reserves issue number 1 even when its record cannot
    # be read, so a later automated issue must not reuse that occupied number.
    assert stats["issueMaxNumber"] == "1"
    assert "artifactCount" not in stats
    assert stats["commitCount"] == "2"
    assert stats["branchCount"] == "1"
    assert stats["pullCount"] == "0"
    assert stats["discussionCount"] == "0"


def test_issue_statistics_reserve_all_numeric_directories_and_default_live(
    installation,
):
    work = installation["work"]
    records = {
        ".forkmesh/issues/open/12/note.txt": "record missing\n",
        ".forkmesh/issues/closed/19/note.txt": "record missing\n",
        ".forkmesh/issues/21/issue-21.json": "{not valid json",
    }
    for relative, contents in records.items():
        target = work / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(contents, encoding="utf-8")
    _run(["git", "add", ".forkmesh"], work)
    _run(["git", "commit", "-m", "add damaged issue metadata"], work)
    _run(["git", "push", "mirror", "main"], work)

    stats = refresh_tool._sample_repository_statistics(
        installation["config"]
    )
    # Qt treats an unreadable open/legacy issue as live, while every numeric
    # tree (including a closed one without its record) reserves its number.
    assert stats["issueCount"] == "2"
    assert stats["issueMaxNumber"] == "21"


def test_repository_statistics_isolate_recursion_error(installation, monkeypatch):
    def recursive_issue_failure(_config):
        raise RecursionError("untrusted issue nesting")

    monkeypatch.setattr(
        refresh_tool,
        "_source_issue_counts",
        recursive_issue_failure,
    )
    stats = refresh_tool._sample_repository_statistics(
        installation["config"]
    )
    assert "issueCount" not in stats
    assert "issueMaxNumber" not in stats
    assert stats["commitCount"] == "1"
    assert stats["branchCount"] == "1"
    assert stats["pullCount"] == "0"
    assert stats["discussionCount"] == "0"
    assert stats["artifactCount"] == "0"


def _accepted_publication_post(
    calls: list[tuple[str, dict]],
    *,
    endpoint_healths: tuple[str, ...] = ("active",),
):
    endpoint_count = 0

    def post(url: str, payload):
        nonlocal endpoint_count
        calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            health = endpoint_healths[
                min(endpoint_count, len(endpoint_healths) - 1)
            ]
            endpoint_count += 1
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": health,
            }
        return 201, {
            "ok": True,
            "repository": {
                "owner": payload["owner"],
                "name": payload["name"],
                "stateHash": payload["stateHash"],
            },
        }

    return post


def test_catalog_semantic_fingerprint_excludes_only_service_churn():
    excluded = {
        "lastSync",
        "updatedAt",
        "cpuPercent",
        "memUsedBytes",
        "memTotalBytes",
        "diskUsedBytes",
        "diskTotalBytes",
        "clonesServed",
        "websiteServed",
        "cloneServedAt",
        "websiteServedAt",
        "cloneServedAgent",
            "websiteServedAgent",
            "activityWeeks",
            "mergeActivityWeeks",
        }
    assert refresh_tool.CATALOG_SEMANTIC_EXCLUDED_FIELDS == excluded
    record = {
        **{field: "first" for field in excluded},
        "owner": "mirror-two",
        "name": "forkmesh",
        "stateHash": "a" * 64,
        "commit": "b" * 40,
        "actionsEnabled": False,
        "actionsState": "disabled",
        "sizeBytes": 123,
        "description": "A living code city",
        "branch": "main",
        "platform": "git",
        "version": "",
        "cloneUrl": "https://worker.example.invalid/mirror-two/forkmesh",
        "channel": "#mirror-two-forkmesh",
        "commitCount": "1",
    }
    report_policy = {"cpu": True, "memory": True, "disk": True}
    fingerprint = refresh_tool._catalog_semantic_fingerprint(
        record, report_policy)
    for field in excluded:
        changed = dict(record)
        changed[field] = "second"
        assert refresh_tool._catalog_semantic_fingerprint(
            changed, report_policy) == fingerprint

    semantic_changes = {
        "stateHash": "c" * 64,
        "commit": "d" * 40,
        "actionsEnabled": True,
        "actionsState": "running",
        "sizeBytes": 456,
        "description": "Changed configuration",
        "branch": "stable",
        "platform": "other",
        "version": "2",
        "cloneUrl": "https://worker.example.invalid/other/repository",
        "channel": "#other",
        "commitCount": "2",
    }
    for field, value in semantic_changes.items():
        changed = dict(record)
        changed[field] = value
        assert refresh_tool._catalog_semantic_fingerprint(
            changed, report_policy) != fingerprint

    # Sample values churn without a write, but the operator's consent to
    # publish each class is itself semantic and must never be coalesced away.
    for field in report_policy:
        changed_policy = dict(report_policy)
        changed_policy[field] = False
        assert refresh_tool._catalog_semantic_fingerprint(
            record, changed_policy) != fingerprint


def test_register_posts_endpoint_then_node_owner_catalog(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = replace(
        installation["config"],
        catalog=replace(
            installation["config"].catalog,
            report_cpu=True,
            report_memory=True,
            report_disk=True,
        ),
    )
    monkeypatch.setattr(
        refresh_tool,
        "_sample_host_telemetry",
        lambda _config: {
            "cpuPercent": 37,
            "memUsedBytes": 300,
            "memTotalBytes": 1000,
            "diskUsedBytes": 800,
            "diskTotalBytes": 2000,
        },
    )
    refresh_tool.refresh(config)
    calls: list[tuple[str, dict]] = []

    def post(url: str, payload):
        calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "active",
            }
        signed_record = dict(payload)
        catalog_signature = signed_record.pop("catalogSig")
        assert signed_record.pop("catalogSigVersion") == 2
        assert signed_record.pop("signature") == ""
        record_hash = hashlib.sha256(
            json.dumps(
                signed_record,
                sort_keys=True,
                separators=(",", ":"),
                ensure_ascii=True,
            ).encode("utf-8")
        ).hexdigest()
        Ed25519PublicKey.from_public_bytes(
            base64.urlsafe_b64decode(
                installation["public"]["nodePublicKey"] + "="
            )
        ).verify(
            base64.urlsafe_b64decode(catalog_signature + "=="),
            ("forkmesh-catalog-v2\n" + record_hash).encode("ascii"),
        )
        return 201, {
            "ok": True,
            "repository": {
                "owner": payload["owner"],
                "name": payload["name"],
                "stateHash": payload["stateHash"],
            },
        }

    result = refresh_tool.register(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    endpoint = calls[0][1]
    assert endpoint["node"] == "mirror-two"
    assert endpoint["baseUrl"] == "https://mirror-two.example.invalid"
    assert endpoint["publicKey"] == installation["public"]["nodePublicKey"]
    assert re_fullmatch_base64_signature(endpoint["signature"])

    catalog = calls[1][1]
    expected_commit = _run(
        ["git", "--git-dir", str(installation["bare"]), "rev-parse", "main"]
    )
    assert catalog["owner"] == "mirror-two"
    assert catalog["name"] == "forkmesh"
    assert catalog["visibility"] == "public"
    assert catalog["commit"] == expected_commit
    assert catalog["commitCount"] == "1"
    assert catalog["branchCount"] == "1"
    assert catalog["issueCount"] == "0"
    assert catalog["issueMaxNumber"] == "0"
    assert catalog["pullCount"] == "0"
    assert catalog["discussionCount"] == "0"
    assert catalog["artifactCount"] == "0"
    assert catalog["actionsEnabled"] is False
    assert catalog["actionsState"] == "disabled"
    assert not {
        "actionsVariables",
        "actionsCommand",
        "actionsWorkingDirectory",
        "actionsLogs",
    }.intersection(catalog)
    assert catalog["cpuPercent"] == 37
    assert (catalog["memUsedBytes"], catalog["memTotalBytes"]) == (300, 1000)
    assert (catalog["diskUsedBytes"], catalog["diskTotalBytes"]) == (800, 2000)
    assert catalog["catalogSigVersion"] == 2
    assert catalog["maintainer"] == installation["public"]["nodePublicKey"]
    assert re_fullmatch_base64_signature(catalog["catalogSig"])
    assert re_fullmatch_base64_signature(catalog["stateSig"])
    assert result == {
        "ok": True,
        "event": "registration_complete",
        "aliasCount": 2,
    }
    state_path = (
        config.gateway_config_path.parent
        / refresh_tool.CATALOG_RENEWAL_STATE_FILE
    )
    assert stat.S_IMODE(state_path.stat().st_mode) == 0o600
    state = json.loads(state_path.read_text(encoding="utf-8"))
    assert set(state) == {
        "schemaVersion",
        "type",
        "node",
        "repository",
        "semanticFingerprint",
        "verifiedAt",
    }
    assert state["schemaVersion"] == 1
    assert state["type"] == "forkmesh.catalog-renewal-state"
    assert state["node"] == "mirror-two"
    assert state["repository"] == "forkmesh"
    assert refresh_tool.SHA256_RE.fullmatch(state["semanticFingerprint"])
    assert isinstance(state["verifiedAt"], int) and state["verifiedAt"] > 0


def test_renew_republishes_valid_active_generation_without_full_preflight(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    calls: list[tuple[str, dict]] = []

    def post(url: str, payload):
        calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "active",
            }
        return 201, {
            "ok": True,
            "repository": {
                "owner": payload["owner"],
                "name": payload["name"],
                "stateHash": payload["stateHash"],
            },
        }

    def forbidden(*_args, **_kwargs):
        raise AssertionError("renew must not run the full gateway preflight")

    monkeypatch.setattr(refresh_tool, "_fsck_source", forbidden)
    monkeypatch.setattr(refresh_tool, "_invoke_gateway_check", forbidden)

    result = refresh_tool.renew(config, post_json=post)

    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    expected_commit = _run(
        ["git", "--git-dir", str(installation["bare"]), "rev-parse", "main"]
    )
    assert calls[1][1]["commit"] == expected_commit
    assert result == {
        "ok": True,
        "event": "renewal_complete",
        "aliasCount": 2,
    }


def test_renew_coalesces_catalog_until_interval_or_semantic_change(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = replace(
        installation["config"],
        catalog=replace(
            installation["config"].catalog,
            report_cpu=True,
            report_memory=True,
            report_disk=True,
        ),
    )
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    telemetry = [{
        "cpuPercent": 10,
        "memUsedBytes": 100,
        "memTotalBytes": 1000,
        "diskUsedBytes": 200,
        "diskTotalBytes": 2000,
    }]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    monkeypatch.setattr(
        refresh_tool,
        "_sample_host_telemetry",
        lambda _config: dict(telemetry[0]),
    )
    calls: list[tuple[str, dict]] = []
    post = _accepted_publication_post(calls)

    refresh_tool.register(config, post_json=post)
    calls.clear()
    now_ms[0] += 4 * 60 * 1000
    telemetry[0] = {
        "cpuPercent": 90,
        "memUsedBytes": 900,
        "memTotalBytes": 1000,
        "diskUsedBytes": 1800,
        "diskTotalBytes": 2000,
    }
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == ["https"]

    calls.clear()
    now_ms[0] = 1_784_840_000_000 + 30 * 60 * 1000
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]

    calls.clear()
    now_ms[0] += 60 * 1000
    changed = replace(
        config,
        catalog=replace(config.catalog, description="A changed catalog"),
    )
    refresh_tool.renew(changed, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]


def test_disabling_host_reporting_immediately_removes_public_telemetry(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    enabled = replace(
        installation["config"],
        catalog=replace(
            installation["config"].catalog,
            report_cpu=True,
            report_memory=True,
            report_disk=True,
        ),
    )
    disabled = replace(
        enabled,
        catalog=replace(
            enabled.catalog,
            report_cpu=False,
            report_memory=False,
            report_disk=False,
        ),
    )
    refresh_tool.refresh(enabled)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])

    def telemetry(config):
        if not (
            config.catalog.report_cpu
            or config.catalog.report_memory
            or config.catalog.report_disk
        ):
            return {}
        return {
            "cpuPercent": 37,
            "memUsedBytes": 300,
            "memTotalBytes": 1000,
            "diskUsedBytes": 800,
            "diskTotalBytes": 2000,
        }

    monkeypatch.setattr(refresh_tool, "_sample_host_telemetry", telemetry)
    calls: list[tuple[str, dict]] = []
    post = _accepted_publication_post(calls)
    refresh_tool.register(enabled, post_json=post)
    assert calls[1][1]["cpuPercent"] == 37

    calls.clear()
    now_ms[0] += 4 * 60 * 1000
    refresh_tool.renew(disabled, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert not {
        "cpuPercent",
        "memUsedBytes",
        "memTotalBytes",
        "diskUsedBytes",
        "diskTotalBytes",
    }.intersection(calls[1][1])

    calls.clear()
    now_ms[0] += 4 * 60 * 1000
    refresh_tool.renew(disabled, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == ["https"]


def test_changed_pending_renewal_checkpoints_catalog_and_bounds_retries(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    initial_calls: list[tuple[str, dict]] = []
    refresh_tool.register(
        config,
        post_json=_accepted_publication_post(initial_calls),
    )
    state_path = refresh_tool._catalog_renewal_state_path(config)
    initial_state = json.loads(state_path.read_text(encoding="utf-8"))
    changed = replace(
        config,
        catalog=replace(config.catalog, description="A changed catalog"),
    )

    now_ms[0] += 4 * 60 * 1000
    calls: list[tuple[str, dict]] = []
    with pytest.raises(
        refresh_tool.RefreshError,
        match="did not activate signed health",
    ):
        refresh_tool.renew(
            changed,
            post_json=_accepted_publication_post(
                calls,
                endpoint_healths=("pending", "pending"),
            ),
        )
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert calls[0][1] == calls[2][1]
    # Catalog acceptance, not endpoint activation, is the durable checkpoint.
    # A failed activation must never make the 20-second service restart repeat
    # the already accepted catalog write.
    accepted_state = json.loads(state_path.read_text(encoding="utf-8"))
    assert (
        accepted_state["semanticFingerprint"]
        != initial_state["semanticFingerprint"]
    )

    calls.clear()
    now_ms[0] += 20 * 1000
    deferred = refresh_tool.renew(
        changed,
        post_json=_accepted_publication_post(calls),
    )
    assert calls == []
    assert deferred == {
        "ok": False,
        "deferred": True,
        "reason": "previous_renewal_failed",
        "retryAfterMs": 220_000,
        "aliasCount": 2,
        "event": "renewal_deferred",
    }

    calls.clear()
    now_ms[0] += 220 * 1000
    with pytest.raises(
        refresh_tool.RefreshError,
        match="did not activate signed health",
    ):
        refresh_tool.renew(
            changed,
            post_json=_accepted_publication_post(
                calls,
                endpoint_healths=("pending",),
            ),
        )
    # The normal timer retries the endpoint once, but the recent accepted
    # catalog remains coalesced and there is no redundant second endpoint POST.
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == ["https"]


def test_renew_fails_open_and_repairs_corrupt_or_unsafe_catalog_state(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    calls: list[tuple[str, dict]] = []
    post = _accepted_publication_post(calls)
    refresh_tool.register(config, post_json=post)
    state_path = refresh_tool._catalog_renewal_state_path(config)

    state_path.write_text("{not-json\n", encoding="utf-8")
    state_path.chmod(0o600)
    calls.clear()
    now_ms[0] += 60 * 1000
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert stat.S_IMODE(state_path.stat().st_mode) == 0o600
    assert state_path.stat().st_size <= (
        refresh_tool.MAX_CATALOG_RENEWAL_STATE_BYTES
    )

    state_path.chmod(0o640)
    calls.clear()
    now_ms[0] += 60 * 1000
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert stat.S_IMODE(state_path.stat().st_mode) == 0o600
    assert not list(
        state_path.parent.glob(
            "." + refresh_tool.CATALOG_RENEWAL_STATE_FILE + ".*.tmp"
        )
    )


def test_catalog_checkpoint_clock_rollback_fails_open_then_recoalesces(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    original_now = 1_784_840_000_000
    now_ms = [original_now]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    calls: list[tuple[str, dict]] = []
    post = _accepted_publication_post(calls)
    refresh_tool.register(config, post_json=post)

    # A checkpoint from the apparent future can never suppress work after a
    # wall-clock rollback. The remote endpoint timestamp gate is the live
    # authority; locally we conservatively republish and repair the checkpoint.
    calls.clear()
    now_ms[0] = original_now - 60 * 1000
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]

    # Once the clock catches up, the repaired same-semantic checkpoint resumes
    # coalescing instead of remaining in a permanent retry loop.
    calls.clear()
    now_ms[0] = original_now + 60 * 1000
    refresh_tool.renew(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == ["https"]


def test_catalog_state_is_not_written_before_verified_catalog_success(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    state_path = refresh_tool._catalog_renewal_state_path(config)
    backoff_path = refresh_tool._catalog_renewal_backoff_path(config)
    calls: list[tuple[str, dict]] = []

    def reject_catalog(url: str, payload):
        calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "active",
            }
        return 503, {"ok": False}

    with pytest.raises(
        refresh_tool.RefreshError,
        match="catalog publication was not accepted",
    ):
        refresh_tool.renew(config, post_json=reject_catalog)
    assert not state_path.exists()
    assert stat.S_IMODE(backoff_path.stat().st_mode) == 0o600
    backoff = json.loads(backoff_path.read_text(encoding="utf-8"))
    assert set(backoff) == {
        "schemaVersion",
        "type",
        "node",
        "repository",
        "semanticFingerprint",
        "failedAt",
        "retryAfter",
    }
    assert backoff["type"] == "forkmesh.catalog-renewal-backoff"
    assert backoff["retryAfter"] - backoff["failedAt"] == 240_000
    assert not list(backoff_path.parent.glob(
        "." + refresh_tool.CATALOG_RENEWAL_BACKOFF_FILE + ".*.tmp"))
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
    ]

    # systemd retries a failed one-shot after 20 seconds. The accepted-state
    # checkpoint must remain absent, but the exact failed semantic generation
    # gets a bounded local retry hint so that restart cannot hammer D1.
    calls.clear()
    now_ms[0] += 20 * 1000
    deferred = refresh_tool.renew(config, post_json=reject_catalog)
    assert calls == []
    assert deferred["event"] == "renewal_deferred"
    assert deferred["retryAfterMs"] == 220_000

    calls.clear()
    now_ms[0] += 220 * 1000
    with pytest.raises(
        refresh_tool.RefreshError,
        match="catalog publication was not accepted",
    ):
        refresh_tool.renew(config, post_json=reject_catalog)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
    ]


def test_required_semantic_change_bypasses_prior_failure_backoff(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    changed = replace(
        config,
        catalog=replace(config.catalog, description="A changed catalog"),
    )
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    failed_calls: list[tuple[str, dict]] = []

    def reject_catalog(url: str, payload):
        failed_calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "active",
            }
        return 503, {"ok": False}

    with pytest.raises(refresh_tool.RefreshError):
        refresh_tool.renew(config, post_json=reject_catalog)

    now_ms[0] += 20 * 1000
    accepted_calls: list[tuple[str, dict]] = []
    result = refresh_tool.renew(
        changed,
        post_json=_accepted_publication_post(accepted_calls),
    )
    assert result["event"] == "renewal_complete"
    assert [url.rsplit("/", 1)[-1] for url, _ in accepted_calls] == [
        "https",
        "repositories",
        "https",
    ]


def test_deferred_renewal_is_a_successful_service_backoff(
    installation,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
):
    deferred = {
        "ok": False,
        "deferred": True,
        "reason": "previous_renewal_failed",
        "retryAfterMs": 220_000,
        "aliasCount": 2,
        "event": "renewal_deferred",
    }
    monkeypatch.setattr(
        refresh_tool, "load_config", lambda _path: installation["config"])
    monkeypatch.setattr(refresh_tool, "renew", lambda _config: dict(deferred))

    assert refresh_tool.main([
        "--config", str(installation["config_path"]), "renew",
    ]) == 0
    captured = capsys.readouterr()
    assert captured.err == ""
    assert json.loads(captured.out) == deferred


def test_actions_configuration_always_republishes_fresh_catalog(
    installation,
    monkeypatch: pytest.MonkeyPatch,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    now_ms = [1_784_840_000_000]
    monkeypatch.setattr(refresh_tool, "_current_time_ms", lambda: now_ms[0])
    calls: list[tuple[str, dict]] = []
    post = _accepted_publication_post(calls)
    refresh_tool.register(config, post_json=post)

    calls.clear()
    now_ms[0] += 60 * 1000
    result = refresh_tool.configure_actions(
        config,
        {
            "schemaVersion": 1,
            "type": "forkmesh.mirror-actions-catalog-configuration",
            "actionsEnabled": False,
        },
        post_json=post,
    )
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert result["event"] == "actions_configuration_complete"


def test_register_rechecks_pending_health_after_catalog_pin_advances(
    installation,
):
    config = installation["config"]
    refresh_tool.refresh(config)
    calls: list[tuple[str, dict]] = []
    endpoint_calls = 0

    def post(url: str, payload):
        nonlocal endpoint_calls
        calls.append((url, dict(payload)))
        if url.endswith("/api/mirrors/https"):
            endpoint_calls += 1
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "pending" if endpoint_calls == 1 else "active",
            }
        return 201, {
            "ok": True,
            "repository": {
                "owner": payload["owner"],
                "name": payload["name"],
                "stateHash": payload["stateHash"],
            },
        }

    result = refresh_tool.register(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url, _ in calls] == [
        "https",
        "repositories",
        "https",
    ]
    assert calls[0][1] == calls[2][1]
    assert result["event"] == "registration_complete"


def test_register_fails_when_signed_health_stays_pending(installation):
    config = installation["config"]
    refresh_tool.refresh(config)
    calls: list[str] = []

    def post(url: str, payload):
        calls.append(url)
        if url.endswith("/api/mirrors/https"):
            return 201, {
                "ok": True,
                "node": payload["node"],
                "baseUrl": payload["baseUrl"],
                "health": "pending",
            }
        return 201, {
            "ok": True,
            "repository": {
                "owner": payload["owner"],
                "name": payload["name"],
                "stateHash": payload["stateHash"],
            },
        }

    with pytest.raises(
        refresh_tool.RefreshError,
        match="did not activate signed health",
    ):
        refresh_tool.register(config, post_json=post)
    assert [url.rsplit("/", 1)[-1] for url in calls] == [
        "https",
        "repositories",
        "https",
    ]


def test_renew_fails_closed_when_source_refs_changed(installation):
    config = installation["config"]
    refresh_tool.refresh(config)
    (installation["work"] / "README.md").write_text(
        "renewal must not advertise changed refs\n", encoding="utf-8"
    )
    _run(["git", "add", "README.md"], installation["work"])
    _run(["git", "commit", "-m", "unsealed change"], installation["work"])
    _run(["git", "push", "mirror", "main"], installation["work"])

    with pytest.raises(
        refresh_tool.RefreshError,
        match="active mirror does not match the exact source refs",
    ):
        refresh_tool.renew(config, post_json=lambda *_args: pytest.fail(
            "renew must not publish changed refs"))


def re_fullmatch_base64_signature(value: str) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 86
        and len(base64.urlsafe_b64decode(value + "==")) == 64
    )


def test_register_stops_before_catalog_when_endpoint_is_rejected(installation):
    config = installation["config"]
    refresh_tool.refresh(config)
    calls = []

    def reject(url: str, payload):
        calls.append((url, payload))
        return 401, {"ok": False}

    with pytest.raises(refresh_tool.RefreshError, match="endpoint registration"):
        refresh_tool.register(config, post_json=reject)
    assert len(calls) == 1
    assert calls[0][0].endswith("/api/mirrors/https")


def test_config_rejects_permissions_symlinks_and_secret_fields(
    installation,
    tmp_path: Path,
):
    config_path = installation["config_path"]
    config_path.chmod(0o640)
    with pytest.raises(refresh_tool.RefreshError, match="unsafe"):
        refresh_tool.load_config(config_path)
    config_path.chmod(0o600)

    link = tmp_path / "refresh-link.json"
    link.symlink_to(config_path)
    with pytest.raises(refresh_tool.RefreshError, match="symbolic link"):
        refresh_tool.load_config(link)

    prohibited = dict(installation["config_value"])
    prohibited["apiToken"] = "must-not-be-accepted"
    prohibited_path = tmp_path / "prohibited.json"
    prohibited_path.write_text(json.dumps(prohibited), encoding="utf-8")
    prohibited_path.chmod(0o600)
    with pytest.raises(refresh_tool.RefreshError, match="prohibited secret"):
        refresh_tool.load_config(prohibited_path)

    release_store = installation["release_store"]
    release_store.chmod(0o755)
    with pytest.raises(refresh_tool.RefreshError, match="release store"):
        refresh_tool.load_config(config_path)
    release_store.chmod(0o700)


def test_release_store_is_optional_for_existing_refresh_configs(
    installation,
    tmp_path: Path,
):
    legacy = dict(installation["config_value"])
    legacy.pop("releaseStore")
    legacy_path = tmp_path / "legacy-refresh.json"
    legacy_path.write_text(
        json.dumps(legacy, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    legacy_path.chmod(0o600)

    config = refresh_tool.load_config(legacy_path)
    assert config.release_store is None
    rendered = refresh_tool._render_gateway_config(
        config,
        refresh_tool._load_public_identity(config),
        refresh_tool.SealMetadata(
            ciphertext_sha256="a" * 64,
            ciphertext_bytes=100,
            key_reference="forkmesh-headless-age:test",
            expected_refs_sha256="b" * 64,
        ),
        installation["archive"] / ("archive-" + "a" * 64 + ".age"),
    )
    assert all(
        "releaseStore" not in repository
        for repository in rendered["repositories"]
    )


def test_git_fsck_failure_is_caught_by_explicit_check_without_leaking_logs(
    installation,
    capsys: pytest.CaptureFixture[str],
):
    corrupt = installation["bare"] / "objects" / "aa" / ("a" * 38)
    corrupt.parent.mkdir(exist_ok=True)
    corrupt.write_bytes(b"not a git object")
    assert refresh_tool.main(
        ["--config", str(installation["config_path"]), "refresh"]
    ) == 0
    capsys.readouterr()
    result = refresh_tool.main(
        ["--config", str(installation["config_path"]), "check"]
    )
    captured = capsys.readouterr()
    assert result == 2
    assert captured.out == ""
    assert "required local operation failed" in captured.err
    assert str(installation["bare"]) not in captured.err
    assert "mirror-two" not in captured.err
    assert "forkmesh.git" not in captured.err
    assert installation["age_secret"] not in captured.err
    assert installation["gateway_config"].exists()
    assert not list(installation["archive"].glob("archive-*.age"))
