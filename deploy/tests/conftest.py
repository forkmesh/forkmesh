"""Sandbox for deploy/ and tools/next_* tests (AGENTS.md).

* HOME and XDG_{DATA,CONFIG,CACHE}_HOME point into a temp dir, and
  FORKMESH_HOME / FORKMESH_PROJECTS_DIR are dropped, so nothing can resolve
  ~/projects/forkmesh (or the old ~/projects/forkmesh-next).
* FMNEXT_CONFIG_DIR points at an empty temp config dir: the tools never see
  the operator's real vultr.env / cloudflare.env / secrets.env.
* Outbound connections to anything but loopback raise, and `ssh`/`scp`/
  `rsync` on PATH are stubs that fail: no test can reach Vultr, Cloudflare
  or a node, even by mistake.
"""

import ipaddress
import os
import socket
import sys
from pathlib import Path

import pytest

for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)

REPO = Path(__file__).resolve().parents[2]
for extra in (REPO / "tools", REPO / "deploy" / "core"):
    if str(extra) not in sys.path:
        sys.path.insert(0, str(extra))

_real_connect = socket.socket.connect


def _loopback_only(self, address):
    host = address[0] if isinstance(address, tuple) else None
    if host is not None:
        try:
            if not ipaddress.ip_address(host).is_loopback:
                raise PermissionError("tests may not connect to %s" % host)
        except ValueError:
            if host not in ("localhost",):
                raise PermissionError("tests may not resolve %s" % host)
    return _real_connect(self, address)


@pytest.fixture(autouse=True)
def sandbox(tmp_path, monkeypatch):
    home = tmp_path / "home"
    for sub in ("", ".local/share", ".config", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("XDG_DATA_HOME", str(home / ".local/share"))
    monkeypatch.setenv("XDG_CONFIG_HOME", str(home / ".config"))
    monkeypatch.setenv("XDG_CACHE_HOME", str(home / ".cache"))
    config = tmp_path / "fmnext-config"
    config.mkdir()
    monkeypatch.setenv("FMNEXT_CONFIG_DIR", str(config))
    monkeypatch.setenv("FMNEXT_LOGS_DIR", str(tmp_path / "fmnext-logs"))
    monkeypatch.delenv("FMNEXT_DEPLOY_LOCK_FD", raising=False)
    for name in ("VULTR_API_KEY", "CLOUDFLARE_API_TOKEN", "CLOUDFLARE_ZONE_ID",
                 "CLOUDFLARE_ACCOUNT_ID", "AWS_ACCESS_KEY_ID", "AWS_SECRET_ACCESS_KEY",
                 "AWS_SESSION_TOKEN", "AWS_REGION", "AWS_DEFAULT_REGION", "AWS_PROFILE"):
        monkeypatch.delenv(name, raising=False)
    stubs = tmp_path / "stub-bin"
    stubs.mkdir()
    for tool in ("ssh", "scp", "rsync", "wrangler"):
        stub = stubs / tool
        stub.write_text("#!/bin/sh\necho 'stubbed %s in tests' >&2\nexit 99\n" % tool)
        stub.chmod(0o755)
    monkeypatch.setenv("PATH", str(stubs) + os.pathsep + os.environ.get("PATH", ""))
    monkeypatch.setattr(socket.socket, "connect", _loopback_only)
    yield config
