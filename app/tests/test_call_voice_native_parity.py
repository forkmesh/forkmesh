import json
import math
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "call_voice_parity.mjs"
IOS_FIXTURE = ROOT / "native/ios/ForkMeshCore/Tests/FMChatTests/Fixtures/call-voice-parity.json"
ANDROID_FIXTURE = ROOT / "native/android/core/src/test/resources/chat/call-voice-parity.json"
SLACK = 1e-7
REGENERATE = "node tools/call_voice_parity.mjs " + " ".join(
    str(path.relative_to(ROOT)) for path in (IOS_FIXTURE, ANDROID_FIXTURE)
)


def _differences(expected, actual, path="$"):
    if isinstance(expected, dict) and isinstance(actual, dict):
        if set(expected) != set(actual):
            yield f"{path}: keys {sorted(set(expected) ^ set(actual))}"
            return
        for key in expected:
            if key != "hash":
                yield from _differences(expected[key], actual[key], f"{path}.{key}")
    elif isinstance(expected, list) and isinstance(actual, list):
        if len(expected) != len(actual):
            yield f"{path}: length {len(expected)} != {len(actual)}"
            return
        for index, (a, b) in enumerate(zip(expected, actual)):
            yield from _differences(a, b, f"{path}[{index}]")
    elif isinstance(expected, (int, float)) and isinstance(actual, (int, float)) and not isinstance(expected, bool):
        if not math.isclose(expected, actual, rel_tol=0, abs_tol=SLACK):
            yield f"{path}: {expected} != {actual}"
    elif expected != actual:
        yield f"{path}: {expected!r} != {actual!r}"


@pytest.fixture(scope="module")
def regenerated(tmp_path_factory):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    out = tmp_path_factory.mktemp("call-voice") / "call-voice-parity.json"
    result = subprocess.run([node, str(SCRIPT), str(out)], capture_output=True, text=True, timeout=300, check=False)
    assert result.returncode == 0, result.stderr
    return json.loads(out.read_text())


def test_the_committed_fixture_matches_the_js_engine(regenerated):
    committed = json.loads(IOS_FIXTURE.read_text())
    problems = list(_differences(committed, regenerated))
    assert not problems, f"the JS voice engine changed; run `{REGENERATE}` and update the native ports: " + "; ".join(problems[:10])


def test_both_phone_copies_are_the_same_file():
    assert IOS_FIXTURE.read_bytes() == ANDROID_FIXTURE.read_bytes(), f"run `{REGENERATE}`"


def test_the_fixture_covers_every_voice_and_switching(regenerated):
    names = {case["name"] for case in regenerated["cases"]}
    voices = {voice["id"] for voice in regenerated["voices"]}
    assert voices <= names
    assert "off" in names
    assert any(case["switchAt"] for case in regenerated["cases"])
    assert regenerated["latency"] == 1728
