import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
OFFICE = ROOT / "office"
GATE = re.compile(r"get you in", re.I)


def test_office_page_opens_on_a_loading_state_not_the_gate():
    page = (OFFICE / "public" / "index.html").read_text(encoding="utf-8")
    join = page[page.index('<section id="join">'):page.index("</section>")]
    assert not GATE.search(join)
    assert '<h1 id="join-title">Opening your office…</h1>' in join


def test_gate_is_only_rendered_for_a_confirmed_signed_out_session():
    source = (OFFICE / "public" / "features" / "identity.js").read_text(
        encoding="utf-8")
    assert len(GATE.findall(source)) == 1
    branch = source[source.index("} else if (!a) {"):]
    assert GATE.search(branch[:branch.index("} else if (!state.org)")])
    assert "picking" not in source


def test_office_identity_client_suite():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    done = subprocess.run(
        [node, "--test", "tests/identity.test.mjs"], cwd=str(OFFICE),
        capture_output=True, text=True, timeout=120)
    assert done.returncode == 0, done.stdout[-4000:] + done.stderr[-2000:]
