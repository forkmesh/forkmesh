import os
import socket
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SHORTCUT = ROOT / ".forkmesh/shortcuts/clean-forkmesh-temporary-files.sh"


def _fake_gio(bin_dir: Path, *, fail: bool = False) -> None:
    gio = bin_dir / "gio"
    gio.write_text(
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        + ("exit 1\n" if fail else "[ \"$1\" = trash ]\n")
        + (
            ""
            if fail
            else "shift\n"
            "[ \"${1:-}\" = -- ] && shift\n"
            "mv -- \"$1\" \"$FAKE_TRASH/${1##*/}\"\n"
        ),
        encoding="utf-8",
    )
    gio.chmod(0o755)


def test_cleanup_shortcut_only_trashes_recognized_artifacts(tmp_path: Path) -> None:
    temp_root = tmp_path / "tmp-root"
    fake_bin = tmp_path / "bin"
    trash = tmp_path / "trash"
    temp_root.mkdir()
    fake_bin.mkdir()
    trash.mkdir()
    _fake_gio(fake_bin)

    (temp_root / "forkmesh-test-build").mkdir()
    (temp_root / "fm-status.json").write_text("{}", encoding="utf-8")
    replica_fixture = temp_root / "tmp.fixture"
    replica_fixture.mkdir()
    (replica_fixture / "replica.json").write_text("{}", encoding="utf-8")
    (temp_root / "unrelated.txt").write_text("keep", encoding="utf-8")
    unrelated_tmp = temp_root / "tmp.unrelated"
    unrelated_tmp.mkdir()
    (unrelated_tmp / "notes.txt").write_text("keep", encoding="utf-8")
    (temp_root / "forkmesh-single-instance").write_text("keep", encoding="utf-8")

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}:{env['PATH']}"
    env["FAKE_TRASH"] = str(trash)
    result = subprocess.run(
        ["bash", str(SHORTCUT), "--root", str(temp_root), "--include-recent"],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )

    assert "Moved 3 ForkMesh temporary artifacts" in result.stdout
    assert {path.name for path in trash.iterdir()} == {
        "forkmesh-test-build",
        "fm-status.json",
        "tmp.fixture",
    }
    assert (temp_root / "unrelated.txt").read_text(encoding="utf-8") == "keep"
    assert (unrelated_tmp / "notes.txt").read_text(encoding="utf-8") == "keep"
    assert (temp_root / "forkmesh-single-instance").read_text(
        encoding="utf-8"
    ) == "keep"


def test_cleanup_shortcut_trashes_a_stale_single_instance_socket(
    tmp_path: Path,
) -> None:
    temp_root = tmp_path / "tmp-root"
    fake_bin = tmp_path / "bin"
    trash = tmp_path / "trash"
    temp_root.mkdir()
    fake_bin.mkdir()
    trash.mkdir()
    _fake_gio(fake_bin)
    stale_socket = temp_root / "forkmesh-single-instance"
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(stale_socket))
    listener.close()

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}:{env['PATH']}"
    env["FAKE_TRASH"] = str(trash)
    subprocess.run(
        ["bash", str(SHORTCUT), "--root", str(temp_root), "--include-recent"],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )

    assert not stale_socket.exists()
    assert [path.name for path in trash.iterdir()] == ["forkmesh-single-instance"]


def test_cleanup_shortcut_refuses_roots_outside_tmp() -> None:
    result = subprocess.run(
        ["bash", str(SHORTCUT), "--root", "/"],
        capture_output=True,
        text=True,
    )

    assert result.returncode != 0
    assert "Refusing to clean outside /tmp" in result.stderr


def test_cleanup_shortcut_falls_back_to_recoverable_freedesktop_trash(
    tmp_path: Path,
) -> None:
    temp_root = tmp_path / "tmp-root"
    fake_bin = tmp_path / "bin"
    data_home = tmp_path / "data"
    temp_root.mkdir()
    fake_bin.mkdir()
    _fake_gio(fake_bin, fail=True)
    artifact = temp_root / "forkmesh-build-output"
    artifact.write_text("build", encoding="utf-8")

    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}:{env['PATH']}"
    env["XDG_DATA_HOME"] = str(data_home)
    result = subprocess.run(
        ["bash", str(SHORTCUT), "--root", str(temp_root), "--include-recent"],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )

    trashed = list((data_home / "Trash/files").iterdir())
    trash_info = list((data_home / "Trash/info").iterdir())
    assert not artifact.exists()
    assert len(trashed) == 1 and trashed[0].read_text(encoding="utf-8") == "build"
    assert len(trash_info) == 1
    assert f"Path={artifact}" in trash_info[0].read_text(encoding="utf-8")
    assert "recoverable FreeDesktop Trash fallback" in result.stdout
