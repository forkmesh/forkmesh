import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_DIR = WORLD / "scene"
FACADE = WORLD / "world-scene.js"
LOADER = (WORLD / "world-loader.js").read_text(encoding="utf-8")
NODE = shutil.which("node")


def scene_modules():
    return sorted(
        path.relative_to(WORLD).as_posix() for path in SCENE_DIR.rglob("*.js")
    )


def test_facade_only_re_exports_scene_modules():
    facade = FACADE.read_text(encoding="utf-8")
    body = re.sub(r'export \{[^}]*\} from "\./scene/[\w/-]+\.js";', "", facade)
    assert body.strip() == ""
    assert 'from "./scene/core.js"' in facade


def test_every_scene_module_is_modulepreloaded_by_the_loader():
    for module in scene_modules():
        assert '"/world/%s"' % module in LOADER, module
    assert '"/world/world-scene.js"' in LOADER


def test_scene_modules_import_nothing_outside_the_world_directory():
    for module in scene_modules():
        text = (WORLD / module).read_text(encoding="utf-8")
        for target in re.findall(
            r'^(?:import|export) [^;]*? from "([^"]+)";', text, re.M | re.S
        ):
            resolved = ((WORLD / module).parent / target).resolve()
            assert WORLD in resolved.parents, (module, target)
            assert resolved.is_file(), (module, target)


@pytest.mark.skipif(NODE is None, reason="node is required")
def test_facade_evaluates_in_node_with_the_public_api():
    script = """
      const scene = await import(%s);
      console.log(JSON.stringify(Object.keys(scene).sort()));
    """ % json.dumps(FACADE.as_uri())
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    exported = set(json.loads(result.stdout))
    for name in (
        "createWorldScene",
        "QR_MODULE_READY",
        "createSettledGroupCuller",
        "installSettledMatrixUpdate",
        "installShaderCompileGuard",
        "warmMaterialsAsync",
        "createGeometryDeduper",
        "indexGeometry",
        "createAvatarPartInstances",
        "worldQualityProfile",
        "packageSpace",
    ):
        assert name in exported, name


def scene_imports(module):
    text = (WORLD / module).read_text(encoding="utf-8")
    targets = re.findall(
        r'^(?:import|export) [^;]*? from "([^"]+)";', text, re.M | re.S
    )
    resolved = [((WORLD / module).parent / target).resolve() for target in targets]
    return {
        path.relative_to(WORLD).as_posix()
        for path in resolved
        if SCENE_DIR in path.parents
    }


def test_scene_module_imports_have_no_cycles():
    graph = {module: scene_imports(module) for module in scene_modules()}
    state = {}

    def visit(module, trail):
        state[module] = "open"
        for target in sorted(graph[module]):
            assert state.get(target) != "open", " -> ".join(trail + [target])
            if target not in state:
                visit(target, trail + [target])
        state[module] = "done"

    for module in graph:
        if module not in state:
            visit(module, [module])


def test_no_scene_module_is_a_monolith():
    for module in scene_modules():
        lines = (WORLD / module).read_text(encoding="utf-8").count("\n")
        assert lines <= 3000, (module, lines)
