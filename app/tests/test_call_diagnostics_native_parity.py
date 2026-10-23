import json
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "call_diagnostics_parity.mjs"
FIXTURE = ROOT / "native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-diagnostics-parity.json"
REGENERATE = f"node tools/call_diagnostics_parity.mjs {FIXTURE.relative_to(ROOT)}"


@pytest.fixture(scope="module")
def regenerated(tmp_path_factory):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    out = tmp_path_factory.mktemp("call-diagnostics") / "call-diagnostics-parity.json"
    result = subprocess.run([node, str(SCRIPT), str(out)], capture_output=True, text=True, timeout=120, check=False)
    assert result.returncode == 0, result.stderr
    return json.loads(out.read_text())


def test_the_committed_fixture_matches_the_web_diagnostics(regenerated):
    committed = json.loads(FIXTURE.read_text())
    assert committed == regenerated, (
        f"app/public/call-quality-meter.js diagnostics changed; run `{REGENERATE}` and update "
        "native/ios/ForkMeshCore/Sources/FMChat/Direct/CallDiagnostics.swift"
    )


def test_the_fixture_covers_every_problem(regenerated):
    severities = {case["diagnosis"]["severity"] for case in regenerated["cases"]}
    assert severities == {"unknown", "ok", "warn", "bad"}
    issues = {issue["id"] for case in regenerated["cases"] for issue in case["diagnosis"]["issues"]}
    assert issues == {"latency", "loss", "jitter", "bandwidth", "cpu", "sendFps", "receive", "face"}
