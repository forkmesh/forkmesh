"""Filesystem layout of the CPython host inside the repository."""

from __future__ import annotations

import os
import sys
from pathlib import Path

HOST_DIR = Path(__file__).resolve().parent.parent          # app/host
APP_DIR = HOST_DIR.parent                                  # app
REPO_DIR = APP_DIR.parent
SHIMS_DIR = HOST_DIR / "shims"
SRC_BUILD_DIR = APP_DIR / "src_build"


def default_data_dir():
    configured = os.environ.get("FORKMESH_HOST_DATA_DIR")
    if configured:
        return Path(configured)
    return Path.home() / "projects" / "forkmesh" / "data" / "fleet" / "host-dev"


def ensure_import_paths(src_dir=None):
    """Put the shims first, then the staged Worker modules, then the plugins."""
    src_dir = Path(src_dir or SRC_BUILD_DIR)
    wanted = [str(SHIMS_DIR), str(src_dir), str(HOST_DIR), str(src_dir / "plugins")]
    for entry in reversed(wanted):
        while entry in sys.path:
            sys.path.remove(entry)
        sys.path.insert(0, entry)
    return src_dir
