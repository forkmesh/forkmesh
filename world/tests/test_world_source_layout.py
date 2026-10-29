import json
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
APP = REPOSITORY / "app"
WORLD = REPOSITORY / "world"


def test_world_source_tools_and_tests_are_owned_by_world():
    assert (WORLD / "src/world.py").is_file()
    assert (WORLD / "src/world_durable_objects.py").is_file()
    assert (WORLD / "src/community_ads_api.py").is_file()
    assert (WORLD / "tools/build_worker_footprint.py").is_file()
    assert (WORLD / "tools/world_dev_server.py").is_file()
    assert (WORLD / "browser_tests/tests/world.spec.js").is_file()
    assert not list((APP / "src").glob("world*.py"))
    assert not list((APP / "tests").glob("test_world_*.py"))
    assert not list((APP / "tools").glob("*world*.py"))


def test_browser_tests_load_the_vendored_three_not_an_npm_copy():
    app_browser_package = json.loads(
        (APP / "browser_tests/package.json").read_text(encoding="utf-8")
    )
    app_browser_lock = json.loads(
        (APP / "browser_tests/package-lock.json").read_text(encoding="utf-8")
    )
    world_browser_package = json.loads(
        (WORLD / "browser_tests/package.json").read_text(encoding="utf-8")
    )
    world_browser_lock = json.loads(
        (WORLD / "browser_tests/package-lock.json").read_text(encoding="utf-8")
    )
    assert "three" not in app_browser_package["devDependencies"]
    assert "node_modules/three" not in app_browser_lock["packages"]
    assert "three" not in world_browser_package["devDependencies"]
    assert "node_modules/three" not in world_browser_lock["packages"]
    assert (WORLD / "public/world/vendor/three-0.184.0/three.module.min.js").is_file()


def test_app_reexports_world_durable_objects_without_shadowing_capacity():
    entry = (APP / "src/entry.py").read_text(encoding="utf-8")
    assert "async def _world_durable_objects(env):" in entry
    assert "import world_durable_objects as _world_durable_objects_module" in entry
    assert "ForkMeshWorld = _world_durable_objects_module.ForkMeshWorld" in entry
    assert (
        "ForkMeshOfficeRoom = _world_durable_objects_module.ForkMeshOfficeRoom"
        in entry
    )


def test_applied_world_migrations_remain_in_shared_history():
    migrations = sorted((APP / "migrations").glob("*_world_*.sql"))
    assert migrations
    explanation = (WORLD / "README.md").read_text(encoding="utf-8")
    assert "immutable" in explanation
    assert "applied migration sequence" in explanation
    assert "planned World database" in explanation
