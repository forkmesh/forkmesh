import os
import re
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SHORTCUTS = ROOT / ".forkmesh" / "shortcuts"
PLATFORMS = ("android", "linux", "windows", "macos")
sys.path.insert(0, str(ROOT / "app" / "src"))
import workflow_files  # noqa: E402


def _steps(path):
    nodes, _, _, _ = workflow_files.parse_header_file(
        "shortcut", f".forkmesh/shortcuts/{path.name}", path.read_text())
    return [(node["name"], node["params"].get("shortcut", "")) for node in nodes[1:]]


@pytest.mark.parametrize("platform", PLATFORMS)
def test_each_platform_has_a_three_step_deploy_shortcut(platform):
    path = SHORTCUTS / f"deploy-build-{platform}.sh"
    assert os.access(path, os.X_OK)
    assert subprocess.run(["bash", "-n", str(path)]).returncode == 0
    assert _steps(path) == [("Plan the build", ""), ("Build, sign and publish", ""),
                            ("Check the homepage", "")]
    text = path.read_text()
    assert f"--deploy-build {platform} --dry-run \"$@\"" in text
    assert f"--deploy-build {platform} \"$@\"" in text
    assert f"--check-build {platform}" in text


def test_the_new_version_workflow_links_every_platform_in_order():
    path = SHORTCUTS / "deploy-new-version.sh"
    assert os.access(path, os.X_OK)
    assert subprocess.run(["bash", "-n", str(path)]).returncode == 0
    steps = _steps(path)
    assert steps[:3] == [("Cut the new version", ""), ("Wait for the release candidate", ""),
                         ("Reset build numbers to 1", "")]
    assert [link for _, link in steps[3:]] == [
        f"deploy-build-{platform}.sh" for platform in PLATFORMS]
    text = path.read_text()
    assert "tools/forkmesh_release_request.py create" in text
    assert text.index("--wait-candidate") < text.index("--reset-builds") < text.index(
        "deploy-build-android.sh")
    for platform in PLATFORMS:
        assert re.search(rf"^bash \.forkmesh/shortcuts/deploy-build-{platform}\.sh "
                         r'--first-build --tag "\$tag"$', text, re.M)


def test_the_release_host_accepts_every_platform_the_shortcuts_name():
    host = (ROOT / "tools" / "forkmesh-release-host.py").read_text()
    for platform in PLATFORMS:
        assert f'"{platform}":' in host
