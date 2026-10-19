import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import forkmesh_paths  # noqa: E402


def _resolve(home, project=None, **extra):
    environ = {"HOME": str(home), **extra}
    return forkmesh_paths.resolve(project, environ=environ, home=home)


def test_fresh_install_keeps_every_root_in_forkmesh_data(tmp_path):
    layout = _resolve(tmp_path)
    store = tmp_path / "projects" / "forkmesh_data" / "forkmesh"
    assert layout.data_store == tmp_path / "projects" / "forkmesh_data"
    assert layout.store == store
    assert (layout.data, layout.config, layout.cache, layout.state, layout.logs, layout.ssh) == (
        store / "data", store / "config", store / "cache", store / "state", store / "logs",
        store / "ssh")
    assert layout.settings_file == store / "config" / "ForkMesh" / "ForkMesh.conf"
    assert layout.fleet_config == store / "config" / "fleet"
    assert layout.publisher_state == store / "state" / "publisher"
    assert layout.systemd_units == store / "config" / "systemd"
    assert layout.toolchain == tmp_path / "projects" / "forkmesh" / "toolchain"


def test_project_folder_roots_stay_in_use_until_moved(tmp_path):
    home_dir = tmp_path / "projects" / "forkmesh"
    (home_dir / "src" / ".git").mkdir(parents=True)
    for sub in ("data", "ssh", "logs", "cache"):
        (home_dir / sub).mkdir()
    (home_dir / "config" / "ForkMesh").mkdir(parents=True)
    (home_dir / "config" / "fleet").mkdir()
    (home_dir / "state").mkdir()
    (home_dir / "state" / "forkmesh.lock").write_text("1")
    layout = _resolve(tmp_path)
    assert layout.data == home_dir / "data"
    assert layout.config == home_dir / "config"
    assert layout.state == home_dir / "state"
    assert layout.fleet_config == home_dir / "config" / "fleet"

    store = tmp_path / "projects" / "forkmesh_data" / "forkmesh"
    store.mkdir(parents=True)
    for sub in ("data", "config", "ssh", "logs", "cache"):
        (home_dir / sub).rename(store / sub)
        (home_dir / sub).symlink_to(store / sub)
    (store / "state").mkdir()
    layout = _resolve(tmp_path)
    assert layout.data == store / "data" and layout.config == store / "config"
    assert layout.fleet_config == store / "config" / "fleet"
    assert layout.state == home_dir / "state"
    (home_dir / "state" / "forkmesh.lock").unlink()
    (home_dir / "state").rmdir()
    assert _resolve(tmp_path).state == store / "state"


def test_other_projects_use_forkmesh_data_and_a_sandboxed_home_keeps_its_own(tmp_path):
    side = _resolve(tmp_path, "side")
    assert side.data == tmp_path / "projects" / "forkmesh_data" / "side" / "data"
    sandbox = tmp_path / "sandbox" / "forkmesh"
    layout = _resolve(tmp_path, FORKMESH_HOME=str(sandbox))
    assert layout.data == tmp_path / "sandbox" / "forkmesh_data" / "forkmesh" / "data"


def test_shell_twin_agrees(tmp_path):
    script = (f'. "{REPO / "tools" / "forkmesh-paths.sh"}"; '
              'forkmesh_data_store_dir; forkmesh_ssh_dir; forkmesh_tool_logs_dir')
    env = {"HOME": str(tmp_path), "PATH": "/usr/bin:/bin"}
    (tmp_path / "projects" / "forkmesh_data" / "forkmesh").mkdir(parents=True)
    out = subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True,
                         timeout=30, check=True).stdout.split()
    store = tmp_path / "projects" / "forkmesh_data"
    assert out == [str(store), str(store / "forkmesh" / "ssh"),
                   str(store / "forkmesh" / "logs" / "tools")]
    sandbox = tmp_path / "elsewhere" / "forkmesh"
    out = subprocess.run(["bash", "-c", script], env={**env, "FORKMESH_HOME": str(sandbox)},
                         capture_output=True, text=True, timeout=30, check=True).stdout.split()
    assert out[0] == str(tmp_path / "elsewhere" / "forkmesh_data")
