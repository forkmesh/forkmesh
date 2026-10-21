#!/usr/bin/env python3
"""Where ForkMesh keeps its files: the projects layout, for every tool.

This is the Python twin of desktop/src/AppPaths.{h,cpp}; the two must agree.

    ~/projects/                     projects root      (FORKMESH_PROJECTS_DIR)
      forkmesh/                     the home project   (FORKMESH_HOME)
        src/                        source checkout
        bin/ toolchain/ builds/ worktrees/ release/    shared by every project
        data/core/                  identity, config/, cache/, state/, logs/, ssh/
        data/<plugin>/              one folder per plugin that keeps data
      <project>/                    any other project, fully isolated
        data/core/ data/<plugin>/

Every root resolves to its place in the layout when that exists (or when there
is nothing older to preserve) and to the legacy XDG / ~/.forkmesh location
otherwise, so a tool keeps working before, during and after the move. A home
folder that is still a git checkout keeps every root legacy: application data
must never sit where `git add -A` can reach it.

The desktop exports FORKMESH_DATA_DIR / FORKMESH_CONFIG_DIR / FORKMESH_PROJECT
to everything it spawns; those win, so a tool started by a project's desktop
works on that project's files without being told.

Standard library only. `python3 tools/forkmesh_paths.py` prints the resolution.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

HOME_PROJECT = "forkmesh"
PROJECT_FILE = "project.json"
CORE_FOLDER = "core"
PLUGIN_DATA_ENTRIES = {
    "actions": "actions",
    "agents": "agents",
    "agent-accounts": "agents/accounts",
    "agent-images": "agents/images",
    "agent-runtime": "agents/runtime",
    "agent-screenshots": "agents/screenshots",
    "prompt": "agents/prompt",
    "teammate-workspaces": "agents/teammate-workspaces",
    "chat_history": "chat/history",
    "avatars": "chat/avatars",
    "local_mesh": "chat/local-mesh",
    "notes": "notes",
    "vault": "vault",
    "backups": "backups",
    "pull-meta": "pull-requests/meta",
    "pull-review": "pull-requests/review",
    "usage-reminders": "usage/reminders",
    "rss-reader": "rss-reader",
    "secure-drive": "secure-drive",
    "fleet_logs": "logs-hosts/fleet",
    "whisper.cpp": "speech/whisper.cpp",
    "parakeet": "speech/parakeet",
}
PLUGIN_FOLDERS = frozenset(v.split("/")[0] for v in PLUGIN_DATA_ENTRIES.values())


def organized_data_entry(legacy_name: str) -> str:
    return PLUGIN_DATA_ENTRIES.get(legacy_name, f"{CORE_FOLDER}/{legacy_name}")


def _holds_flat_data(data: Path) -> bool:
    try:
        names = [entry.name for entry in data.iterdir()]
    except OSError:
        return False
    return any(name != CORE_FOLDER and name not in PLUGIN_FOLDERS for name in names)
_SLUG = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,46}[a-z0-9])?$")


def is_valid_slug(slug: str) -> bool:
    return bool(_SLUG.match(slug or ""))


def _env(name: str, environ: dict[str, str]) -> str:
    return (environ.get(name) or "").strip()


def _xdg(variable: str, home: Path, fallback: str, environ: dict[str, str]) -> Path:
    value = _env(variable, environ)
    return Path(value) if value else home / fallback


def _xdg_sandbox(home: Path, environ: dict[str, str]) -> bool:
    """An XDG_*_HOME pointing away from its default is a deliberate sandbox."""
    defaults = {"XDG_DATA_HOME": ".local/share", "XDG_CONFIG_HOME": ".config",
                "XDG_CACHE_HOME": ".cache"}
    for variable, fallback in defaults.items():
        value = _env(variable, environ)
        if value and os.path.normpath(value) != os.path.normpath(home / fallback):
            return True
    return False


@dataclass(frozen=True)
class Layout:
    projects_root: Path
    home_dir: Path
    project: str
    project_dir: Path
    reroot_allowed: bool
    reroot_blocked_reason: str
    data: Path
    core: Path
    organized: bool
    config: Path          # QSettings root; the settings file is settings_file
    cache: Path
    state: Path
    logs: Path
    ssh: Path
    # Legacy locations, for migration and for messages.
    legacy_data: Path
    legacy_config: Path
    legacy_cache: Path
    legacy_state: Path

    @property
    def is_home_project(self) -> bool:
        return self.project == HOME_PROJECT

    @property
    def settings_file(self) -> Path:
        return self.config / "ForkMesh" / "ForkMesh.conf"

    @property
    def diagnostics(self) -> Path:
        return self.logs / "diagnostics"

    @property
    def identity_key(self) -> Path:
        return self.core / "identity" / "ed25519.pem"

    def data_path(self, legacy_name: str) -> Path:
        if self.organized:
            return self.data / organized_data_entry(legacy_name)
        return self.data / legacy_name

    def plugin_data(self, plugin: str) -> Path:
        return (self.data if self.organized else self.core) / plugin

    def home_root(self, name: str) -> Path:
        container = self.home_dir / "data"
        if name == "data":
            core = container / CORE_FOLDER
            if core.is_dir() or not _holds_flat_data(container):
                return core
            return container
        modern = container / CORE_FOLDER / name
        previous = self.home_dir / name
        if modern.exists() or not _real(previous):
            return modern
        return previous

    # Shared, home-project-only locations. Each falls back to where the same
    # thing lived before the layout, while that older location still exists.
    @property
    def source(self) -> Path:
        return self.home_dir / "src"

    def _shared(self, name: str, legacy: Path) -> Path:
        modern = self.home_dir / name
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def reroot_allowed_for_home(self) -> bool:
        if self.is_home_project:
            return self.reroot_allowed
        return not (self.home_dir / ".git").exists()

    @property
    def toolchain(self) -> Path:
        return self._shared("toolchain", self.legacy_state / "toolchain")

    @property
    def release_work(self) -> Path:
        return self._shared("release", self.legacy_cache.parent.parent / "forkmesh-release-host")

    @property
    def windows_toolchain(self) -> Path:
        modern = self.home_dir / "toolchain-windows"
        legacy = self.legacy_cache.parent.parent / "forkmesh-win-toolchain"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def fanout_hosts(self) -> Path:
        modern = self.home_root("config") / "fanout" / "hosts"
        legacy = self.legacy_config.parent / "forkmesh-fanout" / "hosts"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def tool_state(self) -> Path:
        """Durable state written by tools (scan journals, observation logs)."""
        modern = self.home_root("state") / "tools"
        legacy = self.legacy_state.parent / ".local" / "state" / "forkmesh"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def secrets(self) -> Path:
        """Operator secrets kept by tools (Cloudflare data keys, bridge keys)."""
        modern = self.home_root("config") / "tools" / "secrets"
        legacy = self.legacy_config.parent / "forkmesh" / "secrets"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def bare_mirror(self) -> Path:
        return self.data_path("mirrors") / "forkmesh-forkmesh.git"

    @property
    def fanout_gateways(self) -> Path:
        return self.fanout_hosts.parent / "gateways"

    @property
    def fanout_github(self) -> Path:
        return self.fanout_hosts.parent / "github"

    @property
    def release_seed_hosts(self) -> Path:
        return self.fanout_hosts.parent / "release-hosts"

    @property
    def publisher_bin(self) -> Path:
        return self.home_dir / "bin" / "publisher"

    @property
    def publisher_state(self) -> Path:
        return self.home_root("state") / "publisher"

    @property
    def action_spool(self) -> Path:
        return self.data_path("actions") / "spool"

    @property
    def systemd_units(self) -> Path:
        return self.home_root("config") / "systemd"

    @property
    def fleet_legacy(self) -> Path:
        return self.projects_root / "forkmesh-next"

    def _fleet(self, root: str, legacy: str) -> Path:
        modern = self.home_root(root) / "fleet"
        old = self.fleet_legacy / legacy
        if modern.exists() or not old.exists():
            return modern
        return old

    @property
    def fleet_config(self) -> Path:
        return self._fleet("config", "config")

    @property
    def fleet_ssh(self) -> Path:
        return self._fleet("ssh", "ssh")

    @property
    def node_deploy_bin(self) -> Path:
        return self.home_dir / "bin" / "node-deploy"

    @property
    def node_deploy_worktree(self) -> Path:
        return self.home_dir / "worktrees" / "node-deploy"

    @property
    def fleet_ssh_key(self) -> Path:
        return self.fleet_ssh / "next_fleet_ed25519"

    @property
    def fleet_data(self) -> Path:
        return self._fleet("data", "data")

    @property
    def fleet_logs(self) -> Path:
        return self._fleet("logs", "logs")

    def fleet_binary(self, name: str, variable: str = "") -> str | None:
        configured = os.environ.get(variable, "").strip() if variable else ""
        import shutil
        if configured:
            return configured if Path(configured).is_file() else shutil.which(name)
        local = self.fleet_data / "bin" / name
        if local.is_file() and os.access(local, os.X_OK):
            return str(local)
        return shutil.which(name)


def _real(path: Path) -> bool:
    """Exists and is not merely a compatibility link into the layout."""
    return path.exists() and not path.is_symlink()


def _pick(modern: Path, legacy: Path | None, allowed: bool, probe: Path | None = None,
          modern_probe: Path | None = None) -> Path:
    if legacy is not None and not allowed:
        return legacy
    if (modern_probe if modern_probe is not None else modern).exists():
        return modern
    if legacy is not None and _real(probe if probe is not None else legacy):
        return legacy
    return modern


def resolve(project: str | None = None, environ: dict[str, str] | None = None,
            home: Path | None = None) -> Layout:
    environ = dict(os.environ if environ is None else environ)
    home = Path(home) if home is not None else Path.home()

    projects_root = Path(_env("FORKMESH_PROJECTS_DIR", environ) or home / "projects")
    home_dir = Path(_env("FORKMESH_HOME", environ) or projects_root / HOME_PROJECT)
    slug = (project or _env("FORKMESH_PROJECT", environ)).strip().lower()
    if not is_valid_slug(slug):
        slug = HOME_PROJECT
    is_home = slug == HOME_PROJECT
    project_dir = home_dir if is_home else projects_root / slug

    legacy_data = _xdg("XDG_DATA_HOME", home, ".local/share", environ) / "ForkMesh" / "ForkMesh"
    legacy_config = _xdg("XDG_CONFIG_HOME", home, ".config", environ) / "ForkMesh"
    legacy_cache = _xdg("XDG_CACHE_HOME", home, ".cache", environ) / "ForkMesh" / "ForkMesh"
    legacy_state = home / ".forkmesh"

    blocked = (project_dir / ".git").exists()
    reason = ""
    if blocked:
        reason = (f"{project_dir} is still a git checkout. Move the checkout into "
                  f"{project_dir}/src (tools/forkmesh_layout.py) before application "
                  f"data can live beside it.")
    elif (is_home and _xdg_sandbox(home, environ)
          and not _env("FORKMESH_HOME", environ)
          and not _env("FORKMESH_PROJECTS_DIR", environ)):
        blocked = True
        reason = ("XDG_DATA_HOME, XDG_CONFIG_HOME or XDG_CACHE_HOME points away from "
                  "its default, so this run keeps its files there. Set FORKMESH_HOME "
                  "to place the projects layout explicitly.")
    allowed = not blocked or not is_home

    def legacy(path: Path) -> Path | None:
        return path if is_home else None

    container = project_dir / "data"
    core_dir = container / CORE_FOLDER

    def pick(name: str, legacy_path: Path | None, probe: Path | None = None,
             modern_probe: str = "", previous_probe: str = "") -> Path:
        modern = core_dir / name
        if legacy_path is not None and not allowed:
            return legacy_path
        if (core_dir / (modern_probe or name)).exists():
            return modern
        previous = project_dir / name
        if previous.is_dir() and not previous.is_symlink() and \
                (project_dir / (previous_probe or name)).exists():
            return previous
        if legacy_path is not None and _real(probe if probe is not None else legacy_path):
            return legacy_path
        return modern

    data = _pick(container, legacy(legacy_data), allowed)
    config = pick("config", legacy(legacy_config.parent), legacy(legacy_config),
                  "config/ForkMesh", "config/ForkMesh")
    cache = pick("cache", legacy(legacy_cache))
    previous_state = project_dir / "state"
    if allowed and previous_state.is_dir() and not previous_state.is_symlink() and \
            (previous_state / "forkmesh.lock").exists():
        state = previous_state
    else:
        state = pick("state", legacy(legacy_state), legacy(legacy_state / "forkmesh.lock"))
    logs = pick("logs", legacy(legacy_state), legacy(legacy_state / "diagnostics"))
    ssh = pick("ssh", legacy(legacy_data / "ssh"))

    # What the desktop that spawned us resolved always wins for its own project.
    spawned_for = _env("FORKMESH_PROJECT", environ) or HOME_PROJECT
    if spawned_for == slug:
        if _env("FORKMESH_DATA_DIR", environ):
            data = Path(_env("FORKMESH_DATA_DIR", environ))
        if _env("FORKMESH_CONFIG_DIR", environ):
            config = Path(_env("FORKMESH_CONFIG_DIR", environ))
    organized = (allowed and (data / CORE_FOLDER).is_dir()) or \
        (allowed and data == container and not _holds_flat_data(data))
    core = data / CORE_FOLDER if organized else data

    return Layout(
        projects_root=projects_root, home_dir=home_dir, project=slug,
        project_dir=project_dir, reroot_allowed=not blocked,
        reroot_blocked_reason=reason, data=data, core=core, organized=organized,
        config=config, cache=cache,
        state=state, logs=logs, ssh=ssh, legacy_data=legacy_data,
        legacy_config=legacy_config, legacy_cache=legacy_cache,
        legacy_state=legacy_state,
    )


def main(argv: list[str]) -> int:
    project = None
    if "--project" in argv:
        project = argv[argv.index("--project") + 1]
    layout = resolve(project)
    rows = [
        ("project", layout.project), ("folder", layout.project_dir),
        ("data", layout.data), ("core", layout.core), ("settings", layout.settings_file),
        ("cache", layout.cache), ("state", layout.state), ("logs", layout.logs),
        ("ssh", layout.ssh), ("source", layout.source),
        ("toolchain", layout.toolchain), ("release", layout.release_work),
        ("fan-out hosts", layout.fanout_hosts), ("tool state", layout.tool_state),
        ("secrets", layout.secrets),
        ("bare mirror", layout.bare_mirror), ("gateways", layout.fanout_gateways),
        ("github mirrors", layout.fanout_github),
        ("publisher", layout.publisher_state), ("fleet config", layout.fleet_config),
        ("fleet ssh key", layout.fleet_ssh_key), ("fleet data", layout.fleet_data),
        ("fleet logs", layout.fleet_logs),
    ]
    for label, value in rows:
        print(f"{label:14} {value}")
    if layout.reroot_blocked_reason:
        print(f"{'note':14} {layout.reroot_blocked_reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
