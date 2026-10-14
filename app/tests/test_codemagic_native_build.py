import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODEMAGIC = ROOT / "codemagic.yaml"


def _workflow(name: str) -> str:
    source = CODEMAGIC.read_text(encoding="utf-8")
    start = source.index(f"\n  {name}:\n")
    following = re.search(r"\n  [a-z0-9-]+:\n", source[start + 1:])
    return source[start:start + 1 + following.start()] if following else source[start:]


def test_ios_native_build_compiles_the_app_and_fails_on_errors():
    workflow = _workflow("ios-native-build")

    for step in ("*native_project", "*native_packages", "*native_archive_unsigned",
                 "*native_core_tests"):
        assert f"      - {step}\n" in workflow, step
    assert "-scheme ForkMesh -configuration Debug" in workflow
    assert "-scheme ForkMeshWatch -configuration Debug" in workflow
    assert workflow.count('exit "$status"') >= 2
    assert "zip -qry \"$CM_BUILD_DIR/ForkMesh-native.app.zip\" ForkMesh.app" in workflow
    assert not re.search(r"^\s*exit 0\s*$", workflow, re.M)
    assert "Cancel probes" not in workflow
    assert "|| true\n          exit" not in workflow


def test_the_unsigned_archive_step_fails_when_the_app_is_missing():
    source = CODEMAGIC.read_text(encoding="utf-8")
    step = source[source.index("    - &native_archive_unsigned"):source.index("    - &native_embed_entitlements\n")]

    assert "set -e" in step
    assert "Products/Applications/ForkMesh.app" in step
    assert "exit 1" in step
