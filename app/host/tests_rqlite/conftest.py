import os
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
HOST = HERE.parent
SRC_ROOT = HERE.parents[2]
for path in (HERE, HOST, SRC_ROOT / "tools", SRC_ROOT / "app" / "src"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

# Same hygiene as app/tests/conftest.py: tests never see the desktop's
# project paths.
for _leaked in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
    os.environ.pop(_leaked, None)

import rqlite_cluster  # noqa: E402

requires_rqlite = pytest.mark.skipif(
    rqlite_cluster.rqlited_binary() is None,
    reason="rqlited not found (set RQLITED or install to data/bin)")


@pytest.fixture(scope="module")
def cluster1():
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    with rqlite_cluster.Cluster(1) as cluster:
        yield cluster


@pytest.fixture(scope="module")
def cluster3():
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    with rqlite_cluster.Cluster(3) as cluster:
        yield cluster


@pytest.fixture(scope="session")
def migrated_image():
    """A SQLite image of a fresh rqlite with every app migration applied by
    tools/rqlite_migrate.py (taken once, loaded into each fresh node)."""
    if rqlite_cluster.rqlited_binary() is None:
        pytest.skip("rqlited not found")
    import time
    import urllib.request

    import rqlite_migrate

    with rqlite_cluster.Cluster(1) as cluster:
        node = cluster.leader()
        started = time.perf_counter()
        report = rqlite_migrate.migrate(node.http, log=lambda *_: None)
        report["wall_seconds"] = time.perf_counter() - started
        with urllib.request.urlopen(node.http + "/db/backup",
                                    timeout=60) as response:
            image = response.read()
    return {"image": image, "report": report}


def load_image(node, image):
    import urllib.request
    request = urllib.request.Request(
        node.http + "/db/load", data=image, method="POST",
        headers={"content-type": "application/octet-stream"})
    with urllib.request.urlopen(request, timeout=60) as response:
        body = response.read()
    return body


@pytest.fixture
def migrated_node(migrated_image):
    with rqlite_cluster.Cluster(1) as cluster:
        node = cluster.leader()
        load_image(node, migrated_image["image"])
        yield node
