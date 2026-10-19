#!/usr/bin/env python3
"""Move a ForkMesh box onto the projects layout.

    python3 tools/forkmesh_layout.py            # plan: read-only, prints every step
    python3 tools/forkmesh_layout.py apply      # do it

What it does, in order, all with rename(2) on one filesystem (instant, no
copy, no extra disk):

  1. moves the source checkout from  ~/projects/forkmesh  to
     ~/projects/forkmesh/src  and repairs every git worktree that pointed at it
  2. moves ForkMesh's box-level folders out of ~/.cache, ~/.config,
     ~/.local/share, ~/.local/state and the home folder into the layout
     (build trees, release workspace, toolchains, tool state, secrets, tokens,
     stray worktrees), leaving a link at each old path unless --no-links
  3. moves the user systemd units into config/systemd (linked back, because
     systemd only reads ~/.config/systemd/user) and points them at src/
  4. points the desktop's saved settings at the moved checkout

It does not move the desktop's own data, settings, cache, diagnostics or ssh
keys: the desktop does that itself at its next start, under its single-instance
lock, as soon as ~/projects/forkmesh is no longer a git checkout. Run
`forkmesh --migrate-layout --dry-run` to preview that half.

`apply` refuses to run while the desktop, its services, action runs or git
operations are live. Every file it edits keeps a `.before-forkmesh-projects`
copy beside it, and every step is safe to repeat.

Standard library only.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402

BACKUP_SUFFIX = ".before-forkmesh-projects"
# Names the layout itself owns inside the home project; never checkout content.
LAYOUT_NAMES = {"src", "bin", "lib", "data", "config", "cache", "state", "logs", "ssh",
                "toolchain", "toolchain-windows", "toolchain-host", "builds",
                "builds-windows", "worktrees", "release", "legacy", "project.json"}


@dataclass
class Step:
    kind: str            # move | edit | run | note
    title: str
    detail: str = ""
    action: object = None   # callable; None for notes


@dataclass
class Plan:
    steps: list[Step] = field(default_factory=list)
    blockers: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def add(self, kind: str, title: str, detail: str = "", action=None) -> None:
        self.steps.append(Step(kind, title, detail, action))


def run(args: list[str], cwd: Path | None = None, check: bool = False) -> subprocess.CompletedProcess:
    return subprocess.run(args, cwd=cwd, text=True, capture_output=True, check=check)


def real(path: Path) -> bool:
    return path.exists() and not path.is_symlink()


def same_filesystem(a: Path, b: Path) -> bool:
    def device(p: Path) -> int:
        while not p.exists():
            p = p.parent
        return p.stat().st_dev
    return device(a) == device(b)


def move(source: Path, target: Path, leave_link: bool) -> str:
    """rename(2) only. Returns a one-word outcome."""
    if source.is_symlink() or not source.exists():
        return "done"
    if target.exists() or target.is_symlink():
        return "SKIP (destination exists)"
    if not same_filesystem(source, target.parent):
        return "SKIP (different disk)"
    target.parent.mkdir(parents=True, exist_ok=True)
    os.rename(source, target)
    if leave_link:
        os.symlink(target, source)
    return "moved"


def edit_file(path: Path, transform) -> str:
    if not path.is_file():
        return "done"
    before = path.read_text(errors="surrogateescape")
    after = transform(before)
    if after == before:
        return "done"
    backup = path.with_name(path.name + BACKUP_SUFFIX)
    if not backup.exists():
        shutil.copy2(path, backup)
    temporary = path.with_name(path.name + ".forkmesh-new")
    temporary.write_text(after, errors="surrogateescape")
    shutil.copymode(path, temporary)
    os.replace(temporary, path)
    return "edited"


def checkout_path_rewriter(old: Path, entries: set[str], home: Path | None = None):
    """Text transform: paths into the old checkout now live under old/src.

    `old` itself becomes old/src only where it is a whole path (followed by a
    quote, backslash, comma, whitespace or end of text). `old/<x>` becomes
    old/src/<x> only when <x> was a top-level entry of the checkout, so paths
    into the layout's own folders (old/data, old/config, ...) are left alone,
    and running it twice changes nothing.
    """
    # The same folder, as unit files and shell snippets spell it.
    spellings = [str(old)]
    if home is not None and home in old.parents:
        relative = old.relative_to(home)
        spellings += [f"%h/{relative}", f"~/{relative}", f"$HOME/{relative}",
                      f"${{HOME}}/{relative}"]
    names = "|".join(sorted((re.escape(n) for n in entries - LAYOUT_NAMES), key=len,
                            reverse=True))
    rules = []
    for spelling in spellings:
        base = re.escape(spelling)
        if names:
            rules.append((re.compile(rf"{base}/(?=(?:{names})(?:[/\"'\\,;:\s]|$))"),
                          f"{spelling}/src/"))
        rules.append((re.compile(rf"{base}(?=[\"'\\,;\s]|$)", re.M), f"{spelling}/src"))

    def transform(text: str) -> str:
        for pattern, replacement in rules:
            text = pattern.sub(lambda _m, r=replacement: r, text)
        return text
    return transform


# --- discovery ---------------------------------------------------------------

def virtualenvs(checkout: Path) -> list[Path]:
    """Python virtualenvs kept inside the checkout (they are never relocatable)."""
    found: list[Path] = []
    skip = {".git", ".worktrees", "node_modules", "build", ".dart_tool"}
    def walk(folder: Path, depth: int) -> None:
        if depth > 4:
            return
        try:
            children = list(folder.iterdir())
        except OSError:
            return
        if any(child.name == "pyvenv.cfg" for child in children):
            found.append(folder)
            return
        for child in children:
            if child.is_dir() and not child.is_symlink() and child.name not in skip:
                walk(child, depth + 1)
    walk(checkout, 0)
    return sorted(found)


def repair_virtualenv(venv: Path, rewrite) -> int:
    """Point a moved virtualenv's launchers at where it lives now.

    A venv records its own absolute path in pyvenv.cfg, in the shebang of every
    console script and in the activate scripts. Only small text files in bin/
    and the cfg are touched; the interpreter links and site-packages are not.
    """
    changed = 0
    candidates = [venv / "pyvenv.cfg"]
    for folder in (venv / "bin", venv / "Scripts"):
        if folder.is_dir():
            candidates += [p for p in folder.iterdir() if p.is_file() and not p.is_symlink()]
    for path in candidates:
        try:
            if path.stat().st_size > 256 * 1024:
                continue
            before = path.read_bytes()
            text = before.decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue          # a binary launcher: nothing textual to repoint
        after = rewrite(text)
        if after != text:
            mode = path.stat().st_mode
            temporary = path.with_name(path.name + ".forkmesh-new")
            temporary.write_text(after)
            os.chmod(temporary, mode)
            os.replace(temporary, path)
            changed += 1
    return changed


def git_worktrees(checkout: Path) -> list[Path]:
    result = run(["git", "-C", str(checkout), "worktree", "list", "--porcelain"])
    if result.returncode != 0:
        return []
    return [Path(line[len("worktree "):]) for line in result.stdout.splitlines()
            if line.startswith("worktree ")]


def live_blockers(home_dir: Path, layout: forkmesh_paths.Layout,
                  use_systemctl: bool = True, moving_checkout: bool = True) -> list[str]:
    blockers: list[str] = []
    lock = layout.state / "forkmesh.lock"
    if lock.is_file():
        pid = (lock.read_text(errors="replace").splitlines() or [""])[0].strip()
        if pid.isdigit() and Path(f"/proc/{pid}").exists():
            blockers.append(f"the ForkMesh desktop is running (pid {pid}); quit it, or "
                            f"`systemctl --user stop forkmesh-source.service`")
    units = (run(["systemctl", "--user", "list-units", "--no-legend", "--plain",
                  "--state=active,activating", "forkmesh*"]).stdout
             if use_systemctl else "")
    for line in units.splitlines():
        name = line.split()[0] if line.split() else ""
        if name:
            blockers.append(f"systemd unit {name} is active; stop it first")
    for marker in (home_dir / ".git" / "index.lock", home_dir / "src" / ".git" / "index.lock"):
        if marker.exists():
            blockers.append(f"a git operation is in progress ({marker})")
    # Anything whose working directory is inside the checkout would be left
    # standing in a folder that no longer exists under that name.
    inside: list[str] = []
    for proc in Path("/proc").glob("[0-9]*") if moving_checkout else []:
        try:
            cwd = Path(os.readlink(proc / "cwd"))
        except OSError:
            continue
        if proc.name == str(os.getpid()) or proc.name == str(os.getppid()):
            continue
        if cwd == home_dir or home_dir in cwd.parents:
            try:
                name = (proc / "comm").read_text().strip()
            except OSError:
                name = "?"
            inside.append(f"{name}({proc.name})")
    if inside:
        shown = ", ".join(inside[:8]) + (" ..." if len(inside) > 8 else "")
        blockers.append(f"{len(inside)} process(es) are working inside {home_dir}: {shown}")
    return blockers


# --- the plan ----------------------------------------------------------------

def build_plan(home: Path, leave_links: bool, use_systemctl: bool = True) -> Plan:
    plan = Plan()
    layout = forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT, home=home)
    home_dir = layout.home_dir
    src = home_dir / "src"
    is_checkout = (home_dir / ".git").exists()
    checkout = home_dir if is_checkout else src
    worktrees_before = git_worktrees(checkout) if (checkout / ".git").exists() else []
    entries = ({p.name for p in home_dir.iterdir()} if is_checkout
               else ({p.name for p in src.iterdir()} if src.is_dir() else set()))

    relocations: list[tuple[Path, Path]] = []   # for worktree repair

    def plan_move(title: str, source: Path, target: Path, link: bool = leave_links) -> None:
        if not real(source):
            return
        relocations.append((source, target))
        plan.add("move", title, f"{source}\n      -> {target}"
                 + ("   (link left behind)" if link else ""),
                 lambda s=source, t=target, l=link: move(s, t, l))

    # 1. the checkout itself ---------------------------------------------------
    if is_checkout:
        staging = home_dir.parent / f".forkmesh-src-moving-{int(time.time())}"
        relocations.append((home_dir, src))

        def move_checkout() -> str:
            if not same_filesystem(home_dir, home_dir.parent):
                return "SKIP (different disk)"
            os.rename(home_dir, staging)
            home_dir.mkdir()
            os.rename(staging, src)
            # Old absolute paths into agent worktrees keep resolving.
            if (src / ".worktrees").is_dir() and leave_links:
                os.symlink(src / ".worktrees", home_dir / ".worktrees")
            return "moved"
        plan.add("move", "Source checkout into src/",
                 f"{home_dir}\n      -> {src}", move_checkout)
    elif not (src / ".git").exists():
        plan.add("note", "No source checkout found",
                 f"neither {home_dir} nor {src} is a git checkout; a binary install "
                 f"has nothing to move here")

    # 2. box-level folders ------------------------------------------------------
    cache = layout.legacy_cache.parent.parent          # ~/.cache
    config = layout.legacy_config.parent               # ~/.config
    share = layout.legacy_data.parent.parent           # ~/.local/share
    state = home / ".local" / "state"
    known = [
        ("Agent and release build trees", cache / "forkmesh-builds", home_dir / "builds"),
        ("Release workspace", cache / "forkmesh-release-host", home_dir / "release"),
        ("Windows cross-build tree", cache / "forkmesh-win-build", home_dir / "builds-windows"),
        ("Windows toolchain", cache / "forkmesh-win-toolchain", home_dir / "toolchain-windows"),
        ("Linker toolchain", share / "forkmesh-toolchain", home_dir / "toolchain-host"),
        ("Tool state", state / "forkmesh", layout.home_root("state") / "tools"),
        ("Fan-out state", state / "forkmesh-fanout", layout.home_root("state") / "fanout"),
        ("Tool secrets and keys", config / "forkmesh", layout.home_root("config") / "tools"),
        ("Fan-out host list", config / "forkmesh-fanout", layout.home_root("config") / "fanout"),
        ("MCP token", home / ".forkmesh_mcp_token", layout.home_root("config") / "mcp_token"),
        ("Offline sync key", home / ".forkmesh-offline.key",
         layout.home_root("config") / "offline.key"),
        ("Manual worktrees", home / "forkmesh-worktrees", home_dir / "worktrees"),
        ("Helper libraries", home / ".local" / "lib" / "forkmesh", home_dir / "lib"),
    ]
    # Launchers and helper scripts on PATH; the links left behind keep PATH working.
    local_bin = home / ".local" / "bin"
    for found in sorted(local_bin.glob("forkmesh*")) if local_bin.is_dir() else []:
        known.append((f"Installed program {found.name}", found, home_dir / "bin" / found.name))
    planned = set()
    for title, source, target in known:
        planned.add(source)
        plan_move(title, source, target)

    # Anything else named for ForkMesh that nobody catalogued lands in legacy/.
    sweeps = [(cache, ["forkmesh*"]), (config, ["forkmesh*", "ForkMesh?*"]),
              (share, ["forkmesh*", "ForkMesh?*"]), (state, ["forkmesh*"]),
              (home, [".forkmesh_*", ".forkmesh-*", "forkmesh-*"])]
    app_owned = {layout.legacy_data.parent, layout.legacy_config, layout.legacy_cache.parent,
                 layout.legacy_state}
    for base, patterns in sweeps:
        for pattern in patterns:
            for found in sorted(base.glob(pattern)) if base.is_dir() else []:
                if found in planned or found in app_owned or not real(found):
                    continue
                planned.add(found)
                area = base.name.lstrip(".") or "home"
                plan_move(f"Uncatalogued: {found.name}", found,
                          home_dir / "legacy" / area / found.name.lstrip("."))

    # Registered git worktrees living outside the project (e.g. ~/pr87-resolve).
    for worktree in worktrees_before:
        if worktree == checkout or home_dir in worktree.parents:
            continue
        if any(worktree == s or s in worktree.parents for s, _ in relocations):
            continue
        # The desktop keeps worktrees of its own inside its data (the pull
        # ledger). They move with that data, by the desktop, not from here.
        if any(root in worktree.parents for root in
               (layout.legacy_data, layout.legacy_cache, layout.legacy_state, layout.data)):
            continue
        plan_move(f"Git worktree {worktree.name}", worktree,
                  home_dir / "worktrees" / worktree.name)

    # Worktree repair: every registered worktree, at its new path.
    def repair() -> str:
        if not (src / ".git").exists():
            return "SKIP (no checkout in src/)"
        moved: list[str] = []
        for worktree in worktrees_before:
            new = worktree
            for source, target in relocations:
                if worktree == source:
                    new = target
                elif source in worktree.parents:
                    new = target / worktree.relative_to(source)
            if new != src and new.exists():
                moved.append(str(new))
        result = run(["git", "-C", str(src), "worktree", "repair", *moved])
        return "repaired" if result.returncode == 0 else f"FAIL {result.stderr.strip()[:200]}"
    if len(worktrees_before) > 1:
        plan.add("run", f"Repair {len(worktrees_before) - 1} git worktree link(s)",
                 "git worktree repair, run from src/", repair)

    # 3. systemd user units ------------------------------------------------------
    unit_dir = config / "systemd" / "user"
    rewrite = checkout_path_rewriter(home_dir, entries, home)
    units = sorted(p for p in unit_dir.glob("forkmesh*") if real(p)) if unit_dir.is_dir() else []
    for unit in units:
        # Our own backups sit beside the units and share their prefix: they
        # move along, but are never edited and never linked back.
        if unit.name.endswith(BACKUP_SUFFIX):
            plan_move(f"Backup {unit.name}", unit,
                      layout.systemd_units / unit.name, link=False)
            continue
        files = [unit] if unit.is_file() else sorted(unit.rglob("*.conf"))
        for file in files:
            if rewrite(file.read_text(errors="surrogateescape")) != file.read_text(
                    errors="surrogateescape"):
                plan.add("edit", f"Point {file.name} at src/", str(file),
                         lambda f=file: edit_file(f, rewrite))
        plan_move(f"systemd unit {unit.name}", unit,
                  layout.systemd_units / unit.name, link=True)
    if units and use_systemctl:
        plan.add("run", "Reload systemd user units", "systemctl --user daemon-reload",
                 lambda: "reloaded" if run(["systemctl", "--user", "daemon-reload"]
                                           ).returncode == 0 else "FAIL")

    # Virtualenvs inside the checkout record their own absolute path. After the
    # move every console script in them points at an interpreter path that is
    # gone ("No such file or directory" from the first tool that uses one).
    def stale_virtualenvs() -> list[Path]:
        where = src if (src / ".git").exists() else home_dir
        stale = []
        for venv in virtualenvs(where):
            cfg = venv / "pyvenv.cfg"
            try:
                text = cfg.read_text(errors="replace")
            except OSError:
                continue
            if is_checkout or rewrite(text) != text:
                stale.append(venv)
        return stale

    def repair_all() -> str:
        total = sum(repair_virtualenv(venv, rewrite) for venv in virtualenvs(src))
        return f"repaired ({total} file(s))"
    stale = stale_virtualenvs()
    if stale:
        plan.add("run", f"Repoint {len(stale)} Python virtualenv(s) at src/",
                 "\n      ".join(str(v) for v in stale[:12]), repair_all)

    # 4. the desktop's saved settings --------------------------------------------
    # Wherever the settings file is by the time this runs (the desktop may or
    # may not have migrated yet), repository records must follow the checkout.
    for settings in {layout.settings_file, layout.legacy_config / "ForkMesh.conf",
                     home_dir / "config" / "ForkMesh" / "ForkMesh.conf",
                     home_dir / "data" / "core" / "config" / "ForkMesh" / "ForkMesh.conf"}:
        if settings.is_file() and is_checkout:
            plan.add("edit", "Point saved repository paths at src/", str(settings),
                     lambda f=settings: edit_file(f, rewrite))

    # 5. what stays for the desktop ------------------------------------------------
    pending = [p for p in (layout.legacy_data, layout.legacy_config, layout.legacy_cache,
                           layout.legacy_state) if real(p)]
    keys = sorted(p.name for p in (home / ".ssh").glob("forkmesh_*")) if (home / ".ssh").is_dir() else []
    if pending or keys:
        listing = "\n      ".join(str(p) for p in pending)
        if keys:
            listing += f"\n      {home / '.ssh'}/{{{', '.join(keys)}}}  (+ ~/.ssh/config rewritten)"
        plan.add("note", "Moved by the desktop itself at its next start",
                 listing + "\n      preview: src/desktop/build/forkmesh --migrate-layout --dry-run")

    # Build trees carry absolute source paths.
    if is_checkout:
        plan.add("note", "Build trees need one reconfigure",
                 "CMake caches record the old source path. Re-run cmake -S src/desktop "
                 "-B <build dir> (the desktop's own Rebuild does this); ccache keeps it quick.")
        claude = home / ".claude" / "projects"
        if (claude / str(home_dir).replace("/", "-")).is_dir():
            plan.add("note", "Claude Code keys its project memory by folder",
                     f"sessions started in {src} use a new key; to keep the existing memory:\n"
                     f"      ln -s {claude / str(home_dir).replace('/', '-')} "
                     f"{claude / str(src).replace('/', '-')}")

    plan.blockers = live_blockers(home_dir, layout, use_systemctl, is_checkout)
    if is_checkout and not same_filesystem(home_dir, home_dir.parent):
        plan.blockers.append(f"{home_dir} is a mount point; the checkout cannot be renamed")
    return plan


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="plan", choices=["plan", "apply"])
    parser.add_argument("--no-links", action="store_true",
                        help="do not leave links at the old locations")
    parser.add_argument("--home", default=str(Path.home()), help=argparse.SUPPRESS)
    parser.add_argument("--no-systemctl", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--ignore-live", action="store_true",
                        help="apply even though something is still running (not advised)")
    args = parser.parse_args(argv)

    plan = build_plan(Path(args.home), leave_links=not args.no_links,
                      use_systemctl=not args.no_systemctl)
    applying = args.command == "apply"
    if plan.blockers:
        print("Cannot apply yet:" if applying else "Would refuse to apply right now:")
        for blocker in plan.blockers:
            print(f"  - {blocker}")
        print()
        if applying and not args.ignore_live:
            return 2

    failures = 0
    for number, step in enumerate(plan.steps, 1):
        outcome = ""
        if applying and step.action is not None:
            try:
                outcome = step.action()
            except OSError as error:
                outcome = f"FAIL {error}"
            failures += outcome.startswith("FAIL")
        tag = {"move": "move", "edit": "edit", "run": "run ", "note": "note"}[step.kind]
        print(f"{number:2}. [{tag}] {step.title}" + (f"  ... {outcome}" if outcome else ""))
        if step.detail:
            print(f"      {step.detail}")
    if not plan.steps:
        print("Nothing to do: this box is already on the projects layout.")
    elif not applying:
        print("\nRead-only plan. Run with `apply` to perform it.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
