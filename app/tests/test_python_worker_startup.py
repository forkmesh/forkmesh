"""Keep Python Worker startup configuration within Cloudflare's limits."""

import re
import subprocess
import sys
import time
import tomllib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_runtime_sdk_is_not_uploaded_as_an_external_python_package():
    """The runtime supplies ``workers``; a lockfile uploads a duplicate SDK.

    ``disable_python_external_sdk`` is the flag that says so to the runtime.
    Drop it and startup fails with ModuleNotFoundError for ``workers`` on any
    start that did not vendor the external package, because the runtime then
    expects an uploaded SDK that this worker deliberately does not ship.
    """
    config = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
    project = tomllib.loads((ROOT / "pyproject.toml").read_text(encoding="utf-8"))

    assert config["compatibility_flags"] == [
        "python_workers",
        "disable_python_external_sdk",
    ]
    # python_no_global_handlers is the default from compatibility date
    # 2025-08-14 on, and current workerd refuses to start when it is still
    # spelled out. The date is what keeps the standard handler model on.
    assert str(config["compatibility_date"]) >= "2025-08-14"
    assert "disable_python_no_global_handlers" not in config["compatibility_flags"]
    assert config["python_modules"]["exclude"] == [
        "**/*.pyc",
        "asgi.py",
        "workers/wsgi.py",
    ]
    assert config["build"]["watch_dir"] == [
        "src", "public", "tools", "../marketplace/catalog.json",
        "../marketplace/packages", "../marketplace/tools", "../world/src",
        "../world/public", "../www/public",
    ]
    build_command = config["build"]["command"]
    assert build_command.index("tools/build_worker_python.py") < (
        build_command.index("build_worker_footprint.py --staged")
    )
    assert "--report dist/worker-footprint.json" in build_command
    # The production account currently uses Workers Free, which rejects a
    # custom CPU limit at deploy time. Route-specific imports stay lazy instead.
    assert "limits" not in config
    assert project["project"]["dependencies"] == []
    assert not (ROOT / "pylock.toml").exists()


def test_worker_build_compacts_python_after_ast_equivalence_check():
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "build_worker_python.py"),
         "--check"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    match = re.fullmatch(
        r"worker python: (\d+) -> (\d+) bytes \(saved (\d+), ([\d.]+)%\)\n",
        result.stdout,
    )
    assert match is not None
    before, after, saved = map(int, match.group(1, 2, 3))
    assert before - after == saved
    assert after < before * 0.8

    script = (ROOT / "tools" / "build_worker_python.py").read_text(
        encoding="utf-8"
    )
    assert 'Path("dashboard_bundle.py")' in script
    assert 'Path("dashboard_shell.py")' in script
    assert 'Path("collaboration.py")' in script
    assert 'Path("mirror_replica.py")' in script
    entry = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    assert "import dashboard_bundle" not in entry
    assert "import dashboard_shell" not in entry


def test_worker_stage_is_content_stable_for_wrangler_dev():
    command = [sys.executable, str(ROOT / "tools" / "build_worker_python.py")]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    entry = ROOT / "src_build" / "entry.py"
    staged_entry = entry.read_text(encoding="utf-8")
    # The runtime-owned WorkerEntrypoint is the production GIL-safety
    # boundary. The exported entrypoint class stays in entry.py for Cloudflare
    # to own its PyProxy lifecycle, while the core route implementation is
    # imported during snapshot creation rather than inside the first request.
    assert "class Default(WorkerEntrypoint)" in staged_entry
    assert "import app_runtime as _app_runtime_module" in staged_entry
    assert "_app_runtime_module._bind_runtime(globals())" in staged_entry
    assert "AppRuntime = _app_runtime_module.Default" in staged_entry
    assert "_LazyModule('app_runtime')" not in staged_entry
    assert "async def on_fetch" not in staged_entry
    assert "async def on_scheduled" not in staged_entry
    assert "async def on_queue" not in staged_entry
    assert "worker_runtime" not in staged_entry
    assert not (ROOT / "src_build" / "worker_runtime.py").exists()
    before = entry.stat().st_mtime_ns
    time.sleep(0.002)
    result = subprocess.run(
        command, cwd=ROOT, check=True, capture_output=True, text=True
    )
    assert "stage is up to date" in result.stdout
    assert entry.stat().st_mtime_ns == before
