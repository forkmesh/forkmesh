import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

APP = Path(__file__).resolve().parents[1]
REPOSITORY = APP.parent
TESTS = APP / "tests"
FMM = APP / "public" / "fmm"
FMM_GO = REPOSITORY / "server" / "rtc" / "fmm"
SUITES = sorted(path.name for path in TESTS.glob("fmm_*.test.mjs"))
PORTED = ("seqnum", "wire", "frag", "fec", "cc", "hop", "jitter", "client")
FIXTURES = {
    "wire": FMM_GO / "wire" / "testdata" / "js-parity.json",
    "frag": FMM_GO / "frag" / "testdata" / "js-parity.json",
    "fec": FMM_GO / "fec" / "testdata" / "js-parity.json",
    "cc": FMM_GO / "cc" / "testdata" / "js-parity.json",
    "hop": FMM_GO / "hop" / "testdata" / "js-parity.json",
    "jitter": FMM_GO / "jitter" / "testdata" / "js-parity.json",
    "client": FMM_GO / "testdata" / "js-parity.json",
}


def _node():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    return node


@pytest.mark.parametrize("suite", SUITES)
def test_fmm_node_suite(suite):
    done = subprocess.run(
        [_node(), "--test", str(TESTS / suite)],
        cwd=str(APP),
        capture_output=True,
        text=True,
        timeout=600,
    )
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-4000:]


def test_every_ported_package_has_a_module_a_fixture_and_a_suite():
    for name in PORTED:
        assert (FMM / f"{name}.js").is_file(), name
    for name, fixture in FIXTURES.items():
        assert fixture.is_file(), fixture
        assert f"fmm_{name}.test.mjs" in SUITES, name
        json.loads(fixture.read_text())
    for name in ("seal", "e2ee"):
        assert f"fmm_{name}.test.mjs" in SUITES, name


def test_the_browser_engine_is_code_only():
    comment = re.compile(r"^\s*(//|/\*)", re.MULTILINE)
    for path in sorted(FMM.glob("*.js")):
        assert not comment.search(path.read_text()), f"{path.name} has a comment; reasons belong in docs/app/fmm-browser.md"


def test_the_worker_reports_the_engine_datagram_ceiling():
    worker = (FMM / "fmm-worker.js").read_text()
    assert "DATAGRAM_CEILING = 1152" in worker
    assert "size - 4" in worker
