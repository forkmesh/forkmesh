#!/usr/bin/env python3
"""Authenticated release-publication regression tests."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess

import pytest


PUBLISHER = (
    Path(__file__).resolve().parents[2] / "tools" / "forkmesh-release-publish.sh"
)


def _run(*args, cwd):
    return subprocess.run(
        [str(PUBLISHER), *map(str, args)],
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )


@pytest.fixture()
def release_repo(tmp_path):
    if shutil.which("openssl") is None:
        pytest.skip("OpenSSL is required for release-signature tests")
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(
        ["git", "config", "user.email", "tests@forkmesh.invalid"],
        cwd=tmp_path,
        check=True,
    )
    subprocess.run(
        ["git", "config", "user.name", "ForkMesh tests"],
        cwd=tmp_path,
        check=True,
    )
    (tmp_path / "source.txt").write_text("release source\n", encoding="utf-8")
    subprocess.run(["git", "add", "source.txt"], cwd=tmp_path, check=True)
    subprocess.run(["git", "commit", "-qm", "source"], cwd=tmp_path, check=True)
    subprocess.run(["git", "tag", "v1.2.3"], cwd=tmp_path, check=True)
    commit = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=tmp_path, text=True
    ).strip()
    asset = tmp_path / "forkmesh-linux-x86_64"
    asset.write_bytes(b"\x7fELF signed release test\n")
    private_key = tmp_path / "publisher-private.pem"
    public_key = tmp_path / "publisher-public.pem"
    subprocess.run(
        [
            "openssl",
            "genpkey",
            "-algorithm",
            "Ed25519",
            "-out",
            str(private_key),
        ],
        check=True,
    )
    subprocess.run(
        [
            "openssl",
            "pkey",
            "-in",
            str(private_key),
            "-pubout",
            "-out",
            str(public_key),
        ],
        check=True,
    )
    return tmp_path, commit, asset, private_key, public_key


def test_publisher_requires_signing_key(release_repo):
    repo, commit, asset, _, _ = release_repo
    result = _run(
        "--tag",
        "v1.2.3",
        "--build-commit",
        commit,
        "--cas-dir",
        repo / "cas",
        asset,
        cwd=repo,
    )
    assert result.returncode != 0
    assert "signing-key" in result.stderr
    assert not (repo / ".forkmesh/releases/latest/release.json").exists()


def test_publisher_signs_manifest_and_binds_checksum_list(release_repo):
    repo, commit, asset, private_key, public_key = release_repo
    result = _run(
        "--tag",
        "v1.2.3",
        "--build-commit",
        commit,
        "--cas-dir",
        repo / "cas",
        "--signing-key",
        private_key,
        asset,
        cwd=repo,
    )
    assert result.returncode == 0, result.stderr

    metadata = repo / ".forkmesh/releases/latest"
    manifest_path = metadata / "release.json"
    signature_path = metadata / "release.json.sig"
    sums_path = metadata / "SHASUMS256.txt"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["schema"] == "forkmesh-release-v2"
    assert manifest["checksums_sha256"] == hashlib.sha256(
        sums_path.read_bytes()
    ).hexdigest()
    assert signature_path.stat().st_size == 64

    verify = subprocess.run(
        [
            "openssl",
            "pkeyutl",
            "-verify",
            "-pubin",
            "-rawin",
            "-inkey",
            str(public_key),
            "-in",
            str(manifest_path),
            "-sigfile",
            str(signature_path),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert verify.returncode == 0, verify.stderr

    manifest_path.write_bytes(manifest_path.read_bytes() + b" ")
    tampered = subprocess.run(
        [
            "openssl",
            "pkeyutl",
            "-verify",
            "-pubin",
            "-rawin",
            "-inkey",
            str(public_key),
            "-in",
            str(manifest_path),
            "-sigfile",
            str(signature_path),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    assert tampered.returncode != 0


def test_publisher_labels_native_installer_platforms(release_repo):
    repo, commit, _, private_key, _ = release_repo
    assets = []
    for name in (
        "forkmesh-linux-x86_64.AppImage",
        "forkmesh-linux-x86_64.AppImage.zsync",
        "forkmesh-windows-x86_64-setup.exe",
        "forkmesh-macos-arm64.dmg",
        "forkmesh-macos-x86_64.dmg",
        "forkmesh-android-universal.apk",
    ):
        path = repo / name
        path.write_bytes((name + "\n").encode())
        assets.append(path)
    result = _run(
        "--tag", "v1.2.3",
        "--build-commit", commit,
        "--cas-dir", repo / "cas",
        "--signing-key", private_key,
        *assets,
        cwd=repo,
    )
    assert result.returncode == 0, result.stderr
    manifest = json.loads(
        (repo / ".forkmesh/releases/latest/release.json").read_text()
    )
    platforms = {
        asset["name"]: (asset["os"], asset["arch"])
        for asset in manifest["assets"]
    }
    assert platforms == {
        "forkmesh-linux-x86_64.AppImage": ("linux", "x86_64"),
        "forkmesh-linux-x86_64.AppImage.zsync": ("linux", "x86_64"),
        "forkmesh-windows-x86_64-setup.exe": ("windows", "x86_64"),
        "forkmesh-macos-arm64.dmg": ("macos", "arm64"),
        "forkmesh-macos-x86_64.dmg": ("macos", "x86_64"),
        "forkmesh-android-universal.apk": ("android", "universal"),
    }


def test_same_release_candidate_accumulates_platform_assets(release_repo):
    repo, commit, linux, private_key, _ = release_repo
    windows = repo / "forkmesh-windows-x86_64-setup.exe"
    windows.write_bytes(b"MZ native setup\n")
    common = (
        "--channel", "candidate-1.2.3",
        "--tag", "v1.2.3",
        "--build-commit", commit,
        "--cas-dir", repo / "cas",
        "--signing-key", private_key,
    )
    first = _run(*common, linux, cwd=repo)
    assert first.returncode == 0, first.stderr
    second = _run(*common, windows, cwd=repo)
    assert second.returncode == 0, second.stderr
    manifest = json.loads(
        (repo / ".forkmesh/releases/candidate-1.2.3/release.json").read_text()
    )
    assert {asset["name"] for asset in manifest["assets"]} == {
        "forkmesh-linux-x86_64",
        "forkmesh-windows-x86_64-setup.exe",
    }
