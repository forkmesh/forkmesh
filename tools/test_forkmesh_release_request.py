#!/usr/bin/env python3
import base64
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

_SANDBOX = tempfile.mkdtemp(prefix="fm-release-request-")
for _name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
              "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
              "FORKMESH_ACCOUNT"):
    os.environ.pop(_name, None)
os.environ["HOME"] = _SANDBOX
os.environ["XDG_DATA_HOME"] = os.path.join(_SANDBOX, "data")
os.environ["XDG_CONFIG_HOME"] = os.path.join(_SANDBOX, "config")
os.environ["XDG_CACHE_HOME"] = os.path.join(_SANDBOX, "cache")

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
_spec = importlib.util.spec_from_file_location(
    "fm_release_request", HERE / "forkmesh_release_request.py")
tool = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tool)


def _repo(tags):
    path = Path(tempfile.mkdtemp(dir=_SANDBOX))
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
    subprocess.run(["git", "init", "-q", str(path)], check=True, env=env)
    subprocess.run(["git", "-C", str(path), "commit", "-q", "--allow-empty", "-m", "x"],
                   check=True, env=env)
    for tag in tags:
        subprocess.run(["git", "-C", str(path), "tag", tag], check=True, env=env)
    return path


def test_next_tag_bumps_highest_semver_patch():
    repo = _repo(["v1.2.9", "v1.2.10", "v1.10.0-rc1", "nightly"])
    assert tool.next_tag(repo) == "v1.2.11"


def test_next_tag_without_semver_tag_refuses():
    with pytest.raises(ValueError):
        tool.next_tag(_repo(["nightly"]))


def test_tag_exists():
    repo = _repo(["v1.0.0"])
    assert tool.tag_exists(repo, "v1.0.0")
    assert not tool.tag_exists(repo, "v1.0.1")


def test_request_is_signed_over_the_relay_canonical_string():
    key = Ed25519PrivateKey.generate()
    body = tool.build_request("create", "v1.2.8", "forkmesh", "jett", key.sign, "1700")
    assert body["ownerAccount"] == "jett"
    assert body["command"] == "release.create"
    assert body["target"] == "release:v1.2.8"
    signed = b"forkmesh-desktop-command-v1\njett\nforkmesh\nrelease.create\nrelease:v1.2.8\n1700"
    pad = "=" * (-len(body["sig"]) % 4)
    key.public_key().verify(base64.urlsafe_b64decode(body["sig"] + pad), signed)


def test_publish_names_the_publish_command():
    key = Ed25519PrivateKey.generate()
    body = tool.build_request("publish", "v1.2.8", "forkmesh", "jett", key.sign, "1")
    assert body["command"] == "release.publish"
