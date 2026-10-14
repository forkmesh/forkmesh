import os
import sys
from pathlib import Path

_APP_SRC = str(Path(__file__).resolve().parents[1] / "src")
if _APP_SRC not in sys.path:
    sys.path.append(_APP_SRC)

# The desktop exports FORKMESH_HOME / FORKMESH_PROJECTS_DIR to every agent it
# launches (desktop/src/AppPaths.cpp). A test that swaps only HOME would still
# resolve ForkMesh paths to the owner's real ~/projects/forkmesh — on 2026-09-22
# an installer reinstall test erased real data/config/ssh that way. Tests never
# see them; a test that needs one sets it on its own subprocess env.
for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)
