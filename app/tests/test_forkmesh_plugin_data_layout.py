from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import forkmesh_paths  # noqa: E402


def _cpp_entries() -> dict[str, str]:
    header = (ROOT / "desktop" / "src" / "AppPaths.h").read_text(encoding="utf-8")
    body = header[header.index("pluginDataEntries()"):header.index("organizedDataEntry(")]
    pairs = re.findall(r'\{QStringLiteral\("([^"]+)"\),\s*QStringLiteral\("([^"]+)"\)\}', body)
    return dict(pairs)


def test_desktop_and_tools_share_one_plugin_table():
    assert _cpp_entries() == forkmesh_paths.PLUGIN_DATA_ENTRIES


def test_unknown_entries_belong_to_core():
    assert forkmesh_paths.organized_data_entry("identity") == "core/identity"
    assert forkmesh_paths.organized_data_entry("chat_history") == "chat/history"


def _env(home: Path) -> dict[str, str]:
    return {"HOME": str(home), "FORKMESH_PROJECTS_DIR": str(home / "projects"),
            "FORKMESH_HOME": str(home / "projects" / "forkmesh")}


def test_previous_layout_resolves_flat_until_moved(tmp_path):
    project = tmp_path / "projects" / "forkmesh"
    (project / "data" / "identity").mkdir(parents=True)
    (project / "data" / "identity" / "ed25519.pem").write_text("KEY")
    (project / "data" / "chat_history").mkdir()
    (project / "config" / "ForkMesh").mkdir(parents=True)
    (project / "config" / "fleet").mkdir()
    (project / "ssh" / "fleet").mkdir(parents=True)
    layout = forkmesh_paths.resolve(environ=_env(tmp_path), home=tmp_path)
    assert not layout.organized
    assert layout.identity_key == project / "data" / "identity" / "ed25519.pem"
    assert layout.data_path("chat_history") == project / "data" / "chat_history"
    assert layout.config == project / "config"
    assert layout.fleet_config == project / "config" / "fleet"
    assert layout.fleet_ssh == project / "ssh" / "fleet"


def test_moved_layout_resolves_into_core_and_plugins(tmp_path):
    project = tmp_path / "projects" / "forkmesh"
    core = project / "data" / "core"
    (core / "identity").mkdir(parents=True)
    (core / "config" / "ForkMesh").mkdir(parents=True)
    (core / "ssh").mkdir()
    (project / "data" / "agents" / "sessions").mkdir(parents=True)
    layout = forkmesh_paths.resolve(environ=_env(tmp_path), home=tmp_path)
    assert layout.organized
    assert layout.core == core
    assert layout.identity_key == core / "identity" / "ed25519.pem"
    assert layout.settings_file == core / "config" / "ForkMesh" / "ForkMesh.conf"
    assert layout.ssh == core / "ssh"
    assert layout.data_path("agents") == project / "data" / "agents"
    assert layout.data_path("agent-accounts") == project / "data" / "agents" / "accounts"
    assert layout.bare_mirror == core / "mirrors" / "forkmesh-forkmesh.git"
    assert layout.fleet_config == core / "config" / "fleet"
    assert layout.fleet_data == core / "fleet"
    assert layout.publisher_state == core / "state" / "publisher"


def test_fresh_box_starts_organized(tmp_path):
    layout = forkmesh_paths.resolve(environ=_env(tmp_path), home=tmp_path)
    project = tmp_path / "projects" / "forkmesh"
    assert layout.organized
    assert layout.config == project / "data" / "core" / "config"
    assert layout.fleet_ssh == project / "data" / "core" / "ssh" / "fleet"
