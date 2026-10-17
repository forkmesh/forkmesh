import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
sys.path.insert(0, str(ROOT / "src"))

import repo_projects  # noqa: E402

TABLE = "\n".join([
    "# folder\tname\taliases",
    "bolt-b\tBolt-B\tBolt,Bolt-on-Cloudflare",
    "bolt-c\tBolt-C\t",
    "bolt-d\tBolt-D\tSelfErase",
    "tools/cli\tCLI\t",
    "-\t-\tBolt-A,Bolt-team",
    "../escape\tBad\t",
])


def setting(legacy=("acme-bolt",)):
    return repo_projects.parse(repo_projects.dump(
        repo_projects.clean("Bolt", TABLE, ",".join(legacy))))


def go(path, query="", owner="acme-bolt"):
    return repo_projects.redirect(path, query, setting(), owner)


def test_table_maps_names_aliases_and_folders_case_insensitively():
    parsed = repo_projects.parse_table(TABLE)
    assert parsed["folders"] == {"bolt-b": "Bolt-B", "bolt-c": "Bolt-C",
                                 "bolt-d": "Bolt-D", "tools/cli": "CLI"}
    assert parsed["names"]["selferase"] == "bolt-d"
    assert parsed["names"]["bolt-a"] == repo_projects.RETIRED
    assert "../escape" not in parsed["names"]


def test_project_addresses_redirect_to_folder_views():
    assert go("/Bolt/Bolt-C") == "/Bolt/tree/bolt-c"
    assert go("/bolt/bolt-c") == "/Bolt/tree/bolt-c"
    assert go("/Bolt/CLI") == "/Bolt/tree/tools/cli"
    assert go("/Bolt/SelfErase") == "/Bolt/tree/bolt-d"
    assert go("/Bolt/Bolt-C/blob/main/src/index.js") == \
        "/Bolt/blob/bolt-c/src/index.js?ref=main"
    assert go("/Bolt/Bolt-C/tree/main") == "/Bolt/tree/bolt-c?ref=main"
    assert go("/Bolt/Bolt-C/commits") == "/Bolt/commits?path=bolt-c"
    assert go("/Bolt/Bolt-C/commits/main") == \
        "/Bolt/commits?path=bolt-c&ref=main"
    assert go("/Bolt/Bolt-C/issues") == "/Bolt/issues?label=bolt-c"
    assert go("/Bolt/Bolt-C/pulls") == "/Bolt/pulls"
    assert go("/Bolt/Bolt-C/docs/setup") == "/Bolt/tree/bolt-c/docs/setup"
    assert go("/Bolt/Bolt-C/raw/main/README.md") == \
        "/api/repo/acme-bolt/Bolt/raw?path=bolt-c%2FREADME.md&ref=main"
    assert go("/Bolt/Bolt-A") == "/Bolt"
    assert go("/Bolt/commit/0123abcd") == "/Bolt/commits?ref=0123abcd"


def test_repository_pages_and_unknown_names_are_left_alone():
    for path in ("/Bolt", "/Bolt/issues", "/Bolt/tree/bolt-c", "/Bolt/pulls/4",
                 "/Bolt/not-a-project", "/other/Bolt-C", "/Bolt/commit/xyz",
                 "/acme-bolt/Bolt", "/acme-bolt/Bolt/tree/bolt-c",
                 "/acme-bolt/forkmesh", "/acme-bolt/Bolt-C.git"):
        assert go(path) is None, path


def test_old_owner_addresses_redirect_to_projects():
    assert go("/Acme-Bolt/Bolt-C") == "/Bolt/Bolt-C"
    assert go("/Acme-Bolt/Bolt-C/blob/main/a.md", "x=1") == \
        "/Bolt/Bolt-C/blob/main/a.md?x=1"
    assert go("/acme-bolt/SelfErase/issues") == "/Bolt/Bolt-D/issues"
    assert go("/Acme-Bolt/Bolt", owner="acme") == "/Bolt/Bolt-B"
    assert go("/Acme-Bolt/Bolt-team") == "/Bolt"


def test_raw_needs_the_owner_and_empty_setting_is_inert():
    assert repo_projects.redirect(
        "/Bolt/Bolt-C/raw/main/a", "", setting(), "") is None
    empty = repo_projects.parse("")
    assert repo_projects.redirect("/Bolt/Bolt-C", "", empty, "x") is None
    assert repo_projects.parse("{bad") == empty


def test_clean_rejects_bad_settings():
    with pytest.raises(repo_projects.SettingError):
        repo_projects.clean("bad name", TABLE, "")
    with pytest.raises(repo_projects.SettingError):
        repo_projects.clean("Bolt", "# only comments\n", "")
    with pytest.raises(repo_projects.SettingError):
        repo_projects.clean("Bolt", TABLE, "Not An Owner!")
    assert repo_projects.clean("", TABLE, "x") == {
        "repo": "", "table": "", "legacy": []}


def test_runtime_checks_projects_before_short_routes():
    source = (ROOT / "src" / "app_runtime.py").read_text()
    assert source.index("self._repo_projects_redirect(url)") < \
        source.index("short_route = await self._repo_short_route(url)")
    entry = (ROOT / "src" / "entry.py").read_text()
    assert 'repo_projects = _LazyModule("repo_projects")' in entry


def test_history_forwards_and_filters_by_path():
    gateway = (REPO / "tools" / "mirror_gateway.py").read_text()
    assert re.search(r'"history": frozenset\(\{[^}]*"path"', gateway)
    assert 'arguments += ["--", path]' in gateway
    entry = (ROOT / "src" / "entry.py").read_text()
    assert 'operation in ("tree", "blob", "raw", "commit", "history")' in entry
    assert 'operation == "history" and query.get("path")' in entry
