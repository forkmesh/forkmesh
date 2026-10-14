import json
import sys
from pathlib import Path

import pytest


TOOLS = Path(__file__).resolve().parents[1] / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import docs_dependencies  # noqa: E402


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


@pytest.fixture
def repository(tmp_path: Path) -> Path:
    write(tmp_path / "app/host/requirements.txt", "# pins\nhttpx==0.28.1\nuvicorn[standard]==0.53.0\npytest==9.1.1\n")
    write(tmp_path / "tools/requirements-ci.txt", "pytest==9.1.1\n")
    write(tmp_path / "app/pyproject.toml", '[project]\ndependencies = ["orjson>=3.12"]\n')
    write(tmp_path / "world/tools/package.json", json.dumps({"devDependencies": {"esbuild": "^0.25.10"}}))
    write(tmp_path / "web/node_modules/left-pad/package.json", json.dumps({"dependencies": {"x": "1"}}))
    write(tmp_path / "server/go.mod",
          "module forkmesh.com/x\n\nrequire (\n\tgithub.com/a/b v1.2.3\n\tgolang.org/x/net v0.1.0 // indirect\n)\n"
          "require github.com/c/d v2.0.0\n")
    write(tmp_path / "desktop/CMakeLists.txt",
          "find_package(Python3 REQUIRED)\nfind_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)\n"
          "find_package(Qt6 6.4 QUIET COMPONENTS DBus)\nfind_package(OpenSSL 3.0 REQUIRED)\n"
          "pkg_check_modules(NGHTTP3 QUIET IMPORTED_TARGET libnghttp3)\n")
    write(tmp_path / "native/android/build.gradle.kts", 'plugins {\n    id("org.jetbrains.kotlin.jvm") version "2.4.20" apply false\n}\n')
    write(tmp_path / "native/android/app/build.gradle.kts",
          'dependencies {\n    implementation(platform("androidx.compose:compose-bom:2026.09.00"))\n'
          '    implementation("androidx.compose.ui:ui")\n}\n')
    write(tmp_path / "world/public/world/vendor/satellite-js-7.1.0.esm.js", "")
    write(tmp_path / "world/public/world/vendor/satellite-js-7.1.0.LICENSE.md", "")
    write(tmp_path / "app/public/assets/vendor/three-0.184.0/three.module.min.js", "")
    write(tmp_path / "www/public/assets/vendor/threeui-crt.js", "")
    write(tmp_path / "native/ios/Package.swift",
          '.package(url: "https://github.com/apple/swift-log.git", from: "1.5.0"),\n')
    return tmp_path


def test_inventory_reads_every_manifest_kind(repository: Path):
    entries = {(entry["ecosystem"], entry["name"]): entry for entry in docs_dependencies.inventory(repository)}
    assert set(entries) == {
        ("cpp", "Qt6"), ("cpp", "OpenSSL"), ("cpp", "libnghttp3"),
        ("python", "httpx"), ("python", "uvicorn"), ("python", "pytest"), ("python", "orjson"),
        ("go", "github.com/a/b"), ("go", "golang.org/x/net"), ("go", "github.com/c/d"),
        ("npm", "esbuild"),
        ("vendored", "satellite-js"), ("vendored", "three"),
        ("maven", "org.jetbrains.kotlin.jvm"), ("maven", "androidx.compose:compose-bom"),
        ("maven", "androidx.compose.ui:ui"),
        ("swift", "swift-log"),
    }
    assert entries[("cpp", "Qt6")]["title"] == "Qt 6"
    assert entries[("cpp", "Qt6")]["version"] == "6.4+"
    assert entries[("cpp", "Qt6")]["repository"] == "https://github.com/qt/qtbase"
    assert entries[("python", "pytest")]["usedBy"] == ["app/host/requirements.txt", "tools/requirements-ci.txt"]
    assert entries[("python", "pytest")]["development"] is False
    assert entries[("npm", "esbuild")]["version"] == "0.25.10"
    assert entries[("npm", "esbuild")]["development"] is True
    assert entries[("go", "github.com/c/d")]["version"] == "2.0.0"
    assert entries[("maven", "androidx.compose.ui:ui")]["repository"] == "https://github.com/androidx/androidx"
    assert entries[("swift", "swift-log")]["repository"] == "https://github.com/apple/swift-log"
    ecosystems = [entry["ecosystem"] for entry in docs_dependencies.inventory(repository)]
    assert ecosystems == sorted(ecosystems, key=docs_dependencies.ECOSYSTEM_ORDER.index)


@pytest.mark.parametrize("text, expected", [
    ("git+https://github.com/encode/httpx.git", "https://github.com/encode/httpx"),
    ("https://github.com/python-hyper/h2/tree/main/docs", "https://github.com/python-hyper/h2"),
    ("git@github.com:microsoft/playwright.git", "https://github.com/microsoft/playwright"),
    ("https://github.com/sponsors/someone", ""),
    ("https://gitlab.com/a/b", ""),
    ("", ""),
])
def test_github_url(text: str, expected: str):
    assert docs_dependencies.github_url(text) == expected


class FakeClient:
    def __init__(self, routes: dict, limit: int | None = None):
        self.routes = routes
        self.limit = limit
        self.calls = 0
        self.requested: list[str] = []

    def json(self, url: str, *, github: bool = False):
        self.requested.append(url)
        if github:
            self.calls += 1
            if self.limit is not None and self.calls > self.limit:
                raise docs_dependencies.RateLimited(0)
        return self.routes.get(url)


def contributor(login: str, contributions: int, kind: str = "User") -> dict:
    return {"login": login, "contributions": contributions, "type": kind}


def routes() -> dict:
    api = docs_dependencies.GITHUB_API
    return {
        "https://pypi.org/pypi/httpx/json": {
            "info": {"summary": "The next generation HTTP client.",
                     "project_urls": {"Documentation": "https://www.python-httpx.org",
                                      "Source": "https://github.com/encode/httpx"}}},
        f"{api}/repos/encode/httpx/contributors?per_page=20": [
            contributor("dependabot[bot]", 900, "Bot"),
            contributor("tomchristie", 800),
            contributor("florimondmanca", 300),
            contributor("ghost-bot", 200, "Bot"),
            contributor("lovelydinosaur", 100),
            contributor("fourth", 50),
        ],
        f"{api}/repos/qt/qtbase/contributors?per_page=20": {"_forbidden": "too large"},
        f"{api}/users/tomchristie": {"name": "Tom Christie", "html_url": "https://github.com/tomchristie",
                                     "public_repos": 42, "blog": "www.example.org"},
        f"{api}/users/florimondmanca": {"name": None, "public_repos": 7, "blog": ""},
        f"{api}/users/lovelydinosaur": {"name": "Lovely", "public_repos": 1, "blog": "not a site"},
    }


def small_repository(tmp_path: Path) -> Path:
    write(tmp_path / "app/host/requirements.txt", "httpx==0.28.1\nmystery==1.0\n")
    write(tmp_path / "desktop/CMakeLists.txt", "find_package(Qt6 6.4 REQUIRED)\n")
    return tmp_path


def test_build_records_github_repository_and_top_three_human_authors(tmp_path: Path):
    client = FakeClient(routes())
    saved = []
    snapshot, stopped = docs_dependencies.build(small_repository(tmp_path), None, client, log=lambda *_: None,
                                                checkpoint=lambda partial: saved.append(len(partial["dependencies"])))
    assert stopped == ""
    assert saved == [3, 3, 3]
    assert snapshot["schema"] == docs_dependencies.SCHEMA
    entries = {entry["name"]: entry for entry in snapshot["dependencies"]}
    httpx = entries["httpx"]
    assert httpx["repository"] == "https://github.com/encode/httpx"
    assert httpx["description"] == "The next generation HTTP client."
    assert [author["login"] for author in httpx["authors"]] == ["tomchristie", "florimondmanca", "lovelydinosaur"]
    tom = httpx["authors"][0]
    assert tom == {
        "login": "tomchristie", "name": "Tom Christie", "profile": "https://github.com/tomchristie",
        "projects": "https://github.com/tomchristie?tab=repositories", "publicRepos": 42,
        "website": "https://www.example.org", "contributions": 800,
    }
    assert httpx["authors"][1]["name"] == "florimondmanca"
    assert httpx["authors"][2]["website"] == ""
    assert entries["Qt6"]["authors"] == []
    assert "too large" in entries["Qt6"]["authorsNote"]
    assert entries["mystery"]["repository"] == ""
    assert entries["mystery"]["authorsNote"] == "No GitHub repository is known for this dependency."
    assert not any("fourth" in url for url in client.requested)


def test_build_reuses_recent_data_and_survives_rate_limits(tmp_path: Path):
    root = small_repository(tmp_path)
    first, _ = docs_dependencies.build(root, None, FakeClient(routes()), log=lambda *_: None)
    quiet = FakeClient(routes())
    again, stopped = docs_dependencies.build(root, first, quiet, log=lambda *_: None)
    assert stopped == "" and quiet.calls == 0
    assert again["dependencies"] == first["dependencies"]

    limited = FakeClient(routes(), limit=0)
    partial, stopped = docs_dependencies.build(root, first, limited, refresh=True, log=lambda *_: None)
    assert "rate limit" in stopped
    assert {entry["name"]: entry["authors"] for entry in partial["dependencies"]} == {
        entry["name"]: entry["authors"] for entry in first["dependencies"]}

    offline, stopped = docs_dependencies.build(root, first, None)
    assert stopped == "" and offline["dependencies"] == first["dependencies"]


def test_drift_reports_added_removed_and_changed_dependencies(repository: Path):
    snapshot = {"dependencies": docs_dependencies.inventory(repository)}
    assert docs_dependencies.drift(snapshot, repository) == []
    write(repository / "tools/requirements-ci.txt", "pytest==9.2.0\nruff==0.9.0\n")
    problems = docs_dependencies.drift(snapshot, repository)
    assert "missing python ruff" in problems
    assert any(problem.startswith("changed python pytest") for problem in problems)
    (repository / "world/tools/package.json").unlink()
    assert "no longer used: npm esbuild" in docs_dependencies.drift(snapshot, repository)


def test_snapshot_matches_the_repository_manifests():
    snapshot = json.loads(docs_dependencies.SNAPSHOT.read_text(encoding="utf-8"))
    assert snapshot["schema"] == docs_dependencies.SCHEMA
    assert docs_dependencies.drift(snapshot) == [], (
        "run python3 marketplace/tools/docs_dependencies.py to refresh the Docs package's dependency snapshot")
    for entry in snapshot["dependencies"]:
        assert entry["repository"] == "" or entry["repository"].startswith("https://github.com/"), entry["name"]
        assert len(entry["authors"]) <= docs_dependencies.TOP_AUTHORS, entry["name"]
        for author in entry["authors"]:
            assert author["projects"] == f"https://github.com/{author['login']}?tab=repositories"
            assert author["name"]
