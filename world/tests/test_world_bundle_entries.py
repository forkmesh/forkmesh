import re
import shutil
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
SCENE_IMPORT = re.compile(r"""(?:\bfrom\s*|\bimport\s*\(?\s*)["'](?:\./|/world/)(scene/[A-Za-z0-9_./-]+\.js)""")


def build_site_assets():
    tools = ROOT / "app" / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    import build_site_assets as module
    return module


def unbundled_world_files(world):
    return [
        path for path in sorted(world.rglob("*.js"))
        if not path.relative_to(world).as_posix().startswith(("scene/", "vendor/"))
    ]


def test_built_world_resolves_every_scene_import(tmp_path):
    build = build_site_assets()
    try:
        build.esbuild_package()
    except RuntimeError:
        pytest.skip("esbuild is not installed in world/tools/node_modules")
    if build.node_tool("node") is None:
        pytest.skip("node is not installed")
    dist = tmp_path / "dist"
    shutil.copytree(WORLD, dist / "world")
    staged = dist / "world"
    shutil.copyfile(staged / "world.js", staged / "world-runtime.js")
    shutil.copyfile(staged / "world-loader.js", staged / "world.js")
    (staged / "world-loader.js").unlink()
    build.bundle_world_scene(dist)
    built = dist / "world"
    unresolved = []
    for path in unbundled_world_files(built):
        for target in SCENE_IMPORT.findall(path.read_text(encoding="utf-8")):
            if not (built / target).exists():
                unresolved.append(f"{path.relative_to(built).as_posix()} -> {target}")
    assert unresolved == []
    assert (built / "scene" / "resources.js").exists()


def test_built_world_scene_fits_the_mobile_health_bounds(tmp_path):
    build = build_site_assets()
    try:
        build.esbuild_package()
    except RuntimeError:
        pytest.skip("esbuild is not installed in world/tools/node_modules")
    if build.node_tool("node") is None:
        pytest.skip("node is not installed")
    host = ROOT / "app" / "host"
    if str(host) not in sys.path:
        sys.path.insert(0, str(host))
    from forkmesh_host import world_health
    dist = tmp_path / "dist"
    shutil.copytree(WORLD, dist / "world")
    staged = dist / "world"
    shutil.copyfile(staged / "world.js", staged / "world-runtime.js")
    shutil.copyfile(staged / "world-loader.js", staged / "world.js")
    (staged / "world-loader.js").unlink()
    build.bundle_world_scene(dist)
    bounds = {c[0]: c for c in world_health.MOBILE_WORLD_RUNTIME_ASSETS}
    _, _, minimum, maximum = bounds["/world/world-scene.js"]
    size = (staged / "world-scene.js").stat().st_size
    assert minimum <= size <= maximum
    for dependency in world_health.RUNTIME_DEPENDENCIES.values():
        if dependency[0].startswith("/world/scene/"):
            assert (dist / dependency[0].lstrip("/")).exists()
