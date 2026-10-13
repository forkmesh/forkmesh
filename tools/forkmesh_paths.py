#!/usr/bin/env python3
"""Where ForkMesh keeps its files: the projects layout, for every tool.

This is the Python twin of desktop/src/AppPaths.{h,cpp}; the two must agree.

    ~/projects/                     projects root      (FORKMESH_PROJECTS_DIR)
      forkmesh/                     the home project   (FORKMESH_HOME)
        src/                        source checkout
        bin/ toolchain/ builds/ worktrees/ release/    shared by every project
        data/ config/ cache/ state/ logs/ ssh/         this project's own files
      <project>/                    any other project, fully isolated
        data/ config/ cache/ state/ logs/ ssh/

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
        return self.data / "identity" / "ed25519.pem"

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
        modern = self.home_dir / "config" / "fanout" / "hosts"
        legacy = self.legacy_config.parent / "forkmesh-fanout" / "hosts"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def tool_state(self) -> Path:
        """Durable state written by tools (scan journals, observation logs)."""
        modern = self.home_dir / "state" / "tools"
        legacy = self.legacy_state.parent / ".local" / "state" / "forkmesh"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def secrets(self) -> Path:
        """Operator secrets kept by tools (Cloudflare data keys, bridge keys)."""
        modern = self.home_dir / "config" / "tools" / "secrets"
        legacy = self.legacy_config.parent / "forkmesh" / "secrets"
        if self.reroot_allowed_for_home and (modern.exists() or not _real(legacy)):
            return modern
        return legacy

    @property
    def bare_mirror(self) -> Path:
        return self.data / "mirrors" / "forkmesh-forkmesh.git"

    @property
    def fanout_gateways(self) -> Path:
        return self.fanout_hosts.parent / "gateways"

    @property
    def release_seed_hosts(self) -> Path:
        return self.fanout_hosts.parent / "release-hosts"

    @property
    def publisher_bin(self) -> Path:
        return self.home_dir / "bin" / "publisher"

    @property
    def publisher_state(self) -> Path:
        return self.home_dir / "state" / "publisher"

    @property
    def action_spool(self) -> Path:
        return self.data / "actions" / "spool"

    @property
    def systemd_units(self) -> Path:
        return self.home_dir / "config" / "systemd"

    @property
    def fleet_legacy(self) -> Path:
        return self.projects_root / "forkmesh-next"

    def _fleet(self, root: str, legacy: str) -> Path:
        modern = self.home_dir / root / "fleet"
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

    data = _pick(project_dir / "data", legacy(legacy_data), allowed)
    # config/ may hold tool files before the settings folder has moved in.
    config = _pick(project_dir / "config", legacy(legacy_config.parent), allowed,
                   legacy(legacy_config), project_dir / "config" / "ForkMesh")
    cache = _pick(project_dir / "cache", legacy(legacy_cache), allowed)
    state = _pick(project_dir / "state", legacy(legacy_state), allowed,
                  legacy(legacy_state / "forkmesh.lock"))
    logs = _pick(project_dir / "logs", legacy(legacy_state), allowed,
                 legacy(legacy_state / "diagnostics"))
    ssh = _pick(project_dir / "ssh", legacy(legacy_data / "ssh"), allowed)

    # What the desktop that spawned us resolved always wins for its own project.
    spawned_for = _env("FORKMESH_PROJECT", environ) or HOME_PROJECT
    if spawned_for == slug:
        if _env("FORKMESH_DATA_DIR", environ):
            data = Path(_env("FORKMESH_DATA_DIR", environ))
        if _env("FORKMESH_CONFIG_DIR", environ):
            config = Path(_env("FORKMESH_CONFIG_DIR", environ))

    return Layout(
        projects_root=projects_root, home_dir=home_dir, project=slug,
        project_dir=project_dir, reroot_allowed=not blocked,
        reroot_blocked_reason=reason, data=data, config=config, cache=cache,
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
        ("data", layout.data), ("settings", layout.settings_file),
        ("cache", layout.cache), ("state", layout.state), ("logs", layout.logs),
        ("ssh", layout.ssh), ("source", layout.source),
        ("toolchain", layout.toolchain), ("release", layout.release_work),
        ("fan-out hosts", layout.fanout_hosts), ("tool state", layout.tool_state),
        ("secrets", layout.secrets),
        ("bare mirror", layout.bare_mirror), ("gateways", layout.fanout_gateways),
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
