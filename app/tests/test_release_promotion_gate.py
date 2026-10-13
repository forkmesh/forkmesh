#!/usr/bin/env python3
"""Release candidate -> latest promotion gate (tools/forkmesh-release-promote.py)."""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
PROMOTE = ROOT / "tools" / "forkmesh-release-promote.py"
LINUX_BUILD = (
    "forkmesh-linux-x86_64",
    "forkmesh-linux-x86_64.AppImage",
    "forkmesh-linux-x86_64.AppImage.zsync",
    "forkmesh-mirror-node-linux-x86_64",
)
APK = "forkmesh-android-universal.apk"


def _load_gate():
    spec = importlib.util.spec_from_file_location("forkmesh_release_promote", PROMOTE)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


gate = _load_gate()


def _manifest(tag, channel, names):
    return {
        "schema": "forkmesh-release-v2",
        "repo": "forkmesh/forkmesh",
        "tag": tag,
        "tag_commit": "a" * 40,
        "build_commit": "b" * 40,
        "channel": channel,
        "created_at": 1789256057000,
        "checksums_sha256": "c" * 64,
        "assets": [
            {
                "name": name,
                "blob_sha256": f"{index:064x}",
                "size": 10 + index,
                "os": "linux",
                "arch": "x86_64",
            }
            for index, name in enumerate(sorted(names), start=1)
        ],
    }


def _write_channel(repo, channel, tag, names):
    directory = repo / ".forkmesh" / "releases" / channel
    directory.mkdir(parents=True)
    manifest = _manifest(tag, channel, names)
    (directory / "release.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (directory / "release.json.sig").write_bytes(tag.encode().ljust(64, b"\0"))
    (directory / "SHASUMS256.txt").write_text(
        "".join(f'{a["blob_sha256"]}  {a["name"]}\n' for a in manifest["assets"])
    )
    return directory


def _run(repo, *args, **env):
    environment = {
        key: value
        for key, value in os.environ.items()
        if key not in (gate.REQUIRED_ASSETS_ENV, gate.ALLOW_DROPPED_ENV)
    }
    environment.pop("RELEASE_CHANNEL", None)
    environment.update(env)
    return subprocess.run(
        [sys.executable, str(PROMOTE), *args],
        cwd=repo,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )


def _snapshot(directory):
    return {p.name: p.read_bytes() for p in sorted(directory.iterdir())}


# -- decision -------------------------------------------------------------


def test_default_required_set_is_what_the_fleet_builds_today():
    assert gate.parse_required_assets(None) == {
        "forkmesh-linux-x86_64",
        "forkmesh-linux-x86_64.AppImage",
        "forkmesh-mirror-node-linux-x86_64",
    }


def test_required_set_accepts_commas_and_whitespace():
    assert gate.parse_required_assets(
        " forkmesh-linux-x86_64,forkmesh-macos-arm64.dmg\n forkmesh-windows-x86_64-setup.exe ,"
    ) == {
        "forkmesh-linux-x86_64",
        "forkmesh-macos-arm64.dmg",
        "forkmesh-windows-x86_64-setup.exe",
    }


@pytest.mark.parametrize("value", ["", "   ", ",", " , \n"])
def test_required_set_that_names_nothing_fails_closed(value):
    with pytest.raises(gate.GateError):
        gate.parse_required_assets(value)
    with pytest.raises(gate.GateError):
        gate.decide(_manifest("v1", "c", LINUX_BUILD), None, frozenset(), False)


def test_linux_build_promotes_over_a_release_with_the_same_assets():
    decision = gate.decide(
        _manifest("v0.8.6", "candidate-0.8.6", LINUX_BUILD),
        _manifest("v0.8.3", "latest", LINUX_BUILD),
        gate.parse_required_assets(None),
        allow_dropped=False,
    )
    assert decision.promote
    assert decision.missing_required == [] and decision.dropped == []


def test_partial_matrix_is_held_back():
    decision = gate.decide(
        _manifest("v0.8.6", "candidate-0.8.6", ["forkmesh-linux-x86_64"]),
        None,
        gate.parse_required_assets(None),
        allow_dropped=True,
    )
    assert not decision.promote
    assert decision.missing_required == [
        "forkmesh-linux-x86_64.AppImage",
        "forkmesh-mirror-node-linux-x86_64",
    ]


def test_dropping_a_public_asset_needs_an_explicit_override():
    candidate = _manifest("v0.8.6", "candidate-0.8.6", LINUX_BUILD)
    latest = _manifest("v0.8.3", "latest", [*LINUX_BUILD, APK])
    required = gate.parse_required_assets(None)
    held = gate.decide(candidate, latest, required, allow_dropped=False)
    assert not held.promote and held.dropped == [APK]
    allowed = gate.decide(candidate, latest, required, allow_dropped=True)
    assert allowed.promote and allowed.dropped_allowed


# -- end to end -----------------------------------------------------------


@pytest.fixture()
def repo(tmp_path):
    if shutil.which("bash") is None:
        pytest.skip("bash is required for appcast generation")
    return tmp_path


def test_candidate_missing_latest_apk_is_held_with_publish_instructions(repo):
    _write_channel(repo, "candidate-0.8.6", "v0.8.6", LINUX_BUILD)
    latest = _write_channel(repo, "latest", "v0.8.3", [*LINUX_BUILD, APK])
    before = _snapshot(latest)

    result = _run(repo, "--candidate", "candidate-0.8.6")

    assert result.returncode == gate.EXIT_HOLD, result.stderr
    assert APK in result.stdout
    assert "tools/forkmesh-release-publish.sh --channel candidate-0.8.6 --tag v0.8.6" in result.stdout
    assert f"--build-commit {'b' * 40}" in result.stdout
    assert "FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS=1" in result.stdout
    assert _snapshot(latest) == before


def test_promotion_copies_signed_metadata_and_regenerates_appcast(repo):
    candidate = _write_channel(repo, "candidate-0.8.6", "v0.8.6", [*LINUX_BUILD, APK])
    latest = _write_channel(repo, "latest", "v0.8.3", [*LINUX_BUILD, APK])
    (latest / "appcast.xml").write_text("stale")

    dry = _run(repo, "--candidate", "candidate-0.8.6", "--dry-run")
    assert dry.returncode == 0, dry.stderr
    assert (latest / "appcast.xml").read_text() == "stale"

    result = _run(repo, "--candidate", "candidate-0.8.6", "--channel", "latest")

    assert result.returncode == 0, result.stdout + result.stderr
    for name in gate.METADATA_FILES:
        assert (latest / name).read_bytes() == (candidate / name).read_bytes()
    assert not list(latest.glob(".*.promote.tmp"))
    appcast = (latest / "appcast.xml").read_text()
    assert "<title>ForkMesh (latest)</title>" in appcast
    assert "forkmesh-linux-x86_64.AppImage" in appcast
    assert "<sparkle:version>0.8.6</sparkle:version>" in appcast


def test_override_env_allows_a_deliberate_drop(repo):
    _write_channel(repo, "candidate-0.8.6", "v0.8.6", LINUX_BUILD)
    latest = _write_channel(repo, "latest", "v0.8.3", [*LINUX_BUILD, APK])

    result = _run(
        repo, "--candidate", "candidate-0.8.6",
        FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS="1",
    )

    assert result.returncode == 0, result.stdout + result.stderr
    assert "WARNING" in result.stdout and APK in result.stdout
    assert json.loads((latest / "release.json").read_text())["tag"] == "v0.8.6"


def test_configured_required_asset_missing_holds_even_the_first_release(repo):
    _write_channel(repo, "candidate-0.8.6", "v0.8.6", LINUX_BUILD)

    result = _run(
        repo, "--candidate", "candidate-0.8.6",
        FORKMESH_RELEASE_REQUIRED_ASSETS="forkmesh-linux-x86_64 forkmesh-macos-arm64.dmg",
    )

    assert result.returncode == gate.EXIT_HOLD
    assert "forkmesh-macos-arm64.dmg" in result.stdout
    assert not (repo / ".forkmesh" / "releases" / "latest").exists()


def test_first_release_promotes_without_a_latest_manifest(repo):
    _write_channel(repo, "candidate-0.8.6", "v0.8.6", LINUX_BUILD)

    result = _run(repo, "--candidate", "candidate-0.8.6")

    assert result.returncode == 0, result.stdout + result.stderr
    assert (repo / ".forkmesh" / "releases" / "latest" / "release.json.sig").is_file()


def test_empty_required_variable_and_bad_manifests_fail_closed(repo):
    assert _run(repo, "--check-config").returncode == 0
    assert _run(repo, "--check-config", FORKMESH_RELEASE_REQUIRED_ASSETS=" , ").returncode == 2

    _write_channel(repo, "candidate-0.8.6", "v0.8.6", LINUX_BUILD)
    empty = _run(repo, "--candidate", "candidate-0.8.6", FORKMESH_RELEASE_REQUIRED_ASSETS="")
    assert empty.returncode == 2 and "names no assets" in empty.stderr

    latest = repo / ".forkmesh" / "releases" / "latest"
    latest.mkdir()
    (latest / "release.json").write_text("{not json")
    corrupt = _run(repo, "--candidate", "candidate-0.8.6")
    assert corrupt.returncode == 2
    assert (latest / "release.json").read_text() == "{not json"

    assert _run(repo, "--candidate", "../latest").returncode == 2
    assert _run(repo, "--candidate", "missing-channel").returncode == 2
