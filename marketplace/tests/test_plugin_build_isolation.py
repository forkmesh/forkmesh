import json
import re
from pathlib import Path

import pytest

MARKETPLACE = Path(__file__).resolve().parents[1]
REPOSITORY = MARKETPLACE.parent
CORE_SOURCE = REPOSITORY / "desktop" / "src"
PACKAGES = MARKETPLACE / "packages"
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
WINDOW_HEADER = re.compile(r"^MainWindow\w*\.h$")
GENERATED_PER_COMMIT = {"ForkMeshVersion.h", "ForkMeshBuildCommit.h"}


def module_plugins():
    plugins = []
    for build in sorted(PACKAGES.glob("*/content/desktop/build.json")):
        descriptor = json.loads(build.read_text(encoding="utf-8"))
        if descriptor.get("coreLinked", False):
            continue
        folder = build.parent
        sources = [folder / name for name in descriptor.get("sources", [])]
        plugins.append((folder.parents[1].name, folder, sources))
    return plugins


def include_closure(folder, sources):
    seen = {}
    pending = [source for source in sources if source.is_file()]
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen[current] = []
        text = current.read_text(encoding="utf-8", errors="ignore")
        for name in INCLUDE.findall(text):
            for base in (current.parent, folder, CORE_SOURCE):
                candidate = (base / name).resolve()
                if candidate.is_file():
                    seen[current].append(candidate)
                    pending.append(candidate)
                    break
            else:
                seen[current].append(Path(name))
    return seen


PLUGINS = module_plugins()


def test_there_are_module_plugins_to_check():
    names = {name for name, _, _ in PLUGINS}
    assert {"calendar", "notes", "timers", "crm"} <= names


@pytest.mark.parametrize("name,folder,sources", PLUGINS,
                         ids=[name for name, _, _ in PLUGINS])
def test_a_module_plugin_never_reaches_the_window_headers(name, folder, sources):
    problems = []
    for including, included in include_closure(folder, sources).items():
        for header in included:
            where = f"{including.relative_to(REPOSITORY)} -> {header.name}"
            if WINDOW_HEADER.match(header.name):
                problems.append(f"{where}: the window header")
            elif header.name in GENERATED_PER_COMMIT:
                problems.append(f"{where}: regenerated on every commit")
            elif header.is_absolute():
                inside_own = header.is_relative_to(folder.resolve())
                inside_core = header.is_relative_to(CORE_SOURCE.resolve())
                if not (inside_own or inside_core):
                    problems.append(f"{where}: another plugin's or a foreign folder")
    assert not problems, (
        f"{name} must rebuild only when its own files or the plugin API change; "
        "see docs/desktop/package-modules.md#plugins-built-apart-from-the-window:\n"
        + "\n".join(problems))


def test_the_core_never_includes_a_module_plugin_header():
    plugin_headers = {}
    for name, folder, _ in PLUGINS:
        for header in folder.rglob("*.h"):
            if not (CORE_SOURCE / header.name).exists():
                plugin_headers[header.name] = name
    problems = []
    for source in sorted(CORE_SOURCE.iterdir()):
        if source.suffix not in {".h", ".cpp"}:
            continue
        for included in INCLUDE.findall(source.read_text(encoding="utf-8", errors="ignore")):
            owner = plugin_headers.get(Path(included).name)
            if owner:
                problems.append(f"desktop/src/{source.name} includes {included} ({owner})")
    assert not problems, "\n".join(problems)
