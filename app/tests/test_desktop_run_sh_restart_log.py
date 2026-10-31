import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_SH = ROOT / "desktop" / "run.sh"
SETUP = ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
SHARED = ROOT / "desktop" / "src" / "MainWindowShared.cpp"

FAKE_CMAKE = """#!/usr/bin/env bash
echo "fake cmake $*"
if [ "$1" = "-B" ]; then
    mkdir -p build
    : > build/CMakeCache.txt
    : > build/build.ninja
fi
if [ "$1" = "--build" ]; then
    if [ -n "${FAKE_CMAKE_FAIL:-}" ]; then
        echo "MainWindow.cpp:12:3: error: expected ';' before '}' token"
        exit 1
    fi
    mkdir -p build
    printf '#!/bin/sh\\necho launched-fake-forkmesh\\n' > build/forkmesh
    chmod +x build/forkmesh
fi
"""


def _sandbox(tmp_path: Path) -> tuple[Path, dict]:
    repo = tmp_path / "repo"
    (repo / "desktop").mkdir(parents=True)
    (repo / "tools").mkdir()
    shutil.copy(RUN_SH, repo / "desktop" / "run.sh")
    shutil.copy(ROOT / "tools" / "forkmesh-safe-rm.sh", repo / "tools" / "forkmesh-safe-rm.sh")
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "cmake").write_text(FAKE_CMAKE)
    (bin_dir / "cmake").chmod(0o755)
    env = {
        key: value
        for key, value in os.environ.items()
        if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_RUN_QUIET")
    }
    home = tmp_path / "home"
    home.mkdir()
    env.update(
        HOME=str(home),
        XDG_DATA_HOME=str(home / "data"),
        XDG_CONFIG_HOME=str(home / "config"),
        XDG_CACHE_HOME=str(home / "cache"),
        PATH=f"{bin_dir}:{env.get('PATH', '/usr/bin:/bin')}",
    )
    return repo, env


def _run(repo: Path, env: dict, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(repo / "desktop" / "run.sh"), *args],
        cwd=repo,
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
    )


def test_run_prints_the_rebuild_restart_steps_and_timings(tmp_path):
    repo, env = _sandbox(tmp_path)
    result = _run(repo, env)
    assert result.returncode == 0, result.stderr
    out, err = result.stdout, result.stderr
    assert "launched-fake-forkmesh" in out
    assert out.index("==> Configuring...") < out.index("$ cmake -B build") < out.index("==> Rebuilding...")
    assert out.index("$ cmake --build build --parallel") < out.index("==> Built in") < out.index("==> Launching...")
    assert re.search(r"^  \(in .*/desktop\)$", out, re.M)
    assert re.search(r"^— finished in \d+ms \(exit 0\)$", out, re.M)
    assert re.search(r"^==> Built in \S+ \(configure \S+, build \S+\)$", out, re.M)
    for line in (
        r"run\.sh run: build & launch started",
        r"run: cmake -B build -DCMAKE_BUILD_TYPE=Release",
        r"done in \d+ms \(exit 0\): cmake --build build",
        r"build pass 1: \d+ parallel jobs, HEAD \S+",
        r"summary: configure \S+, build \S+; total \d+ms",
        r"launching \./build/forkmesh",
    ):
        assert re.search(r"^\[restart \+ *\d+ms\] " + line, err, re.M), line


def test_quiet_keeps_the_old_output(tmp_path):
    repo, env = _sandbox(tmp_path)
    result = _run(repo, env, "--quiet", "run")
    assert result.returncode == 0, result.stderr
    assert "launched-fake-forkmesh" in result.stdout
    assert "[restart" not in result.stderr
    assert "==>" not in result.stdout
    assert "finished in" not in result.stdout


def test_failed_build_reports_the_first_error_and_timings(tmp_path):
    repo, env = _sandbox(tmp_path)
    env["FAKE_CMAKE_FAIL"] = "1"
    result = _run(repo, env)
    assert result.returncode == 1
    assert "launched-fake-forkmesh" not in result.stdout
    assert re.search(r"^— finished in \d+ms \(exit 1\)$", result.stdout, re.M)
    assert "!! Build failed: MainWindow.cpp:12:3: error: expected ';'" in result.stderr
    assert re.search(r"\] ERROR: Build failed: ", result.stderr)
    assert re.search(r"^==> Failed in \S+ \(configure \S+, build \S+\)$", result.stdout, re.M)


def test_wording_matches_the_desktop_rebuild_and_restart_log():
    setup = SETUP.read_text()
    shared = SHARED.read_text()
    run_sh = RUN_SH.read_text()
    assert '"[restart +%1ms] %2"' in shared and "'[restart +%5dms] %s\\n'" in run_sh
    assert '"\\n$ %1\\n  (in %2)\\n"' in setup and "'\\n$ %s\\n  (in %s)\\n'" in run_sh
    assert '"\\xE2\\x80\\x94 finished in %1ms (exit %2)\\n"' in setup
    assert "'\\xE2\\x80\\x94 finished in %sms (exit %s)\\n'" in run_sh
    assert '"done in %1ms (exit %2): %3"' in setup and '"done in ${took}ms (exit $status): $command_line"' in run_sh
    assert '"build pass %1: %2 parallel jobs, HEAD %3"' in setup
    assert '"build pass 1: $1 parallel jobs, HEAD $head"' in run_sh
    for status in ("Configuring...", "Rebuilding..."):
        assert f'"{status}"' in setup and f'fm_status "{status}"' in run_sh


def test_second_run_skips_the_configure_step(tmp_path):
    repo, env = _sandbox(tmp_path)
    assert _run(repo, env).returncode == 0
    result = _run(repo, env)
    assert result.returncode == 0, result.stderr
    assert "launched-fake-forkmesh" in result.stdout
    assert "$ cmake -B build" not in result.stdout
    assert "==> Configuring..." not in result.stdout
    assert re.search(r"^==> Built in \S+ \(build \S+\)$", result.stdout, re.M)
    assert "build tree already configured; skipping the configure step" in result.stderr


def test_a_cache_rewritten_elsewhere_configures_again(tmp_path):
    repo, env = _sandbox(tmp_path)
    assert _run(repo, env).returncode == 0
    cache = repo / "desktop" / "build" / "CMakeCache.txt"
    later = cache.stat().st_mtime + 10
    os.utime(cache, (later, later))
    result = _run(repo, env)
    assert result.returncode == 0, result.stderr
    assert "$ cmake -B build" in result.stdout


def test_changed_configure_arguments_configure_again(tmp_path):
    repo, env = _sandbox(tmp_path)
    assert _run(repo, env).returncode == 0
    stamp = repo / "desktop" / "build" / "run-sh-configure.args"
    stamp.write_text(stamp.read_text().replace("-DFORKMESH_BUILD_TESTS=OFF", "-DFORKMESH_BUILD_TESTS=ON"))
    os.utime(stamp, (stamp.stat().st_mtime + 10,) * 2)
    result = _run(repo, env)
    assert result.returncode == 0, result.stderr
    assert "$ cmake -B build" in result.stdout


def test_launch_starts_the_last_build_without_building(tmp_path):
    repo, env = _sandbox(tmp_path)
    missing = _run(repo, env, "launch")
    assert missing.returncode == 1
    assert "!! Nothing is built in" in missing.stderr
    assert _run(repo, env).returncode == 0
    result = _run(repo, env, "launch")
    assert result.returncode == 0, result.stderr
    assert "launched-fake-forkmesh" in result.stdout
    assert "fake cmake" not in result.stdout
    assert "==> Built in" not in result.stdout
    assert not (repo / "desktop" / "build" / ".run-sh.lock").exists()


def test_the_build_lock_is_released_after_each_mode(tmp_path):
    repo, env = _sandbox(tmp_path)
    lock = repo / "desktop" / "build" / ".run-sh.lock"
    for mode in ("run", "rebuild", "launch", "clean"):
        result = _run(repo, env, mode)
        assert result.returncode == 0, (mode, result.stderr)
        assert not lock.exists(), mode
    assert not (repo / "desktop" / "build").exists()


def test_a_running_build_makes_the_next_one_wait(tmp_path):
    repo, env = _sandbox(tmp_path)
    lock = repo / "desktop" / "build" / ".run-sh.lock"
    lock.mkdir(parents=True)
    holder = subprocess.Popen(["sleep", "30"])
    try:
        (lock / "pid").write_text(f"{holder.pid}\n")
        waiting = subprocess.Popen(
            ["bash", str(repo / "desktop" / "run.sh")],
            cwd=repo, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            waiting.wait(timeout=3)
            raise AssertionError("run.sh did not wait for the running build")
        except subprocess.TimeoutExpired:
            pass
        assert not (repo / "desktop" / "build" / "forkmesh").exists()
        holder.kill()
        holder.wait()
        out, err = waiting.communicate(timeout=30)
    finally:
        holder.kill()
    assert waiting.returncode == 0, err
    assert "==> Waiting for the build already running in" in err
    assert f"(pid {holder.pid})" in err
    assert "removing the stale build lock" in err
    assert "launched-fake-forkmesh" in out
    assert not lock.exists()


def test_a_lock_left_by_a_dead_build_is_taken_over(tmp_path):
    repo, env = _sandbox(tmp_path)
    lock = repo / "desktop" / "build" / ".run-sh.lock"
    lock.mkdir(parents=True)
    dead = subprocess.Popen(["true"])
    dead.wait()
    (lock / "pid").write_text(f"{dead.pid}\n")
    result = _run(repo, env)
    assert result.returncode == 0, result.stderr
    assert "Waiting for the build" not in result.stderr
    assert "launched-fake-forkmesh" in result.stdout
    assert not lock.exists()
