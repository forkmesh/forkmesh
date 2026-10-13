"""Locate forkmesh_integration.py for the examples.

Published usage: download ``tools/forkmesh_integration.py`` next to an example
(or point FORKMESH_INTEGRATION_TOOL at it). Inside a ForkMesh checkout the
repository copy is used.
"""

import importlib.util
import os
from pathlib import Path


def load():
    here = Path(__file__).resolve().parent
    candidates = [
        os.environ.get("FORKMESH_INTEGRATION_TOOL", ""),
        here / "forkmesh_integration.py",
        here.parents[3] / "tools" / "forkmesh_integration.py",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            spec = importlib.util.spec_from_file_location(
                "forkmesh_integration", candidate)
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            return module
    raise SystemExit("forkmesh_integration.py not found; see www/docs/integrations.md")
