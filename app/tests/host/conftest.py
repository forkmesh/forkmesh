"""Shared setup for the CPython host tests (app/host/forkmesh_host).

Puts app/host and the shims on sys.path. Tests that import the Worker entry
module use the ``staged_src`` fixture, which runs
app/tools/build_worker_python.py when app/src_build is missing or stale.
"""

from __future__ import annotations

import importlib
import os
import subprocess
import sys
from pathlib import Path

import pytest

HOST_DIR = Path(__file__).resolve().parents[2] / "host"
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))

from forkmesh_host import paths  # noqa: E402

paths.ensure_import_paths()

for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)


def _stale():
    entry = paths.SRC_BUILD_DIR / "entry.py"
    if not entry.is_file():
        return True
    newest = max(
        p.stat().st_mtime
        for root in (paths.APP_DIR / "src", paths.REPO_DIR / "world" / "src")
        for p in root.glob("*.py"))
    return newest > entry.stat().st_mtime


@pytest.fixture(scope="session")
def staged_src():
    if _stale():
        subprocess.run([sys.executable, "tools/build_worker_python.py"],
                       cwd=paths.APP_DIR, check=True)
        importlib.invalidate_caches()
    return paths.SRC_BUILD_DIR


@pytest.fixture(scope="session")
def sandbox_env():
    return _sandbox_env


def _sandbox_env(tmp_path):
    """Subprocess environment per AGENTS.md: temp HOME/XDG, no FORKMESH_*."""
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")
           and not k.startswith("FORKMESH_VAR_")}
    home = tmp_path / "home"
    for sub in ("", ".local/share", ".config", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(home),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "PYTHONPATH": str(HOST_DIR),
    })
    return env
