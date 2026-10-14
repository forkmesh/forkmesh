#!/usr/bin/env python3
from __future__ import annotations

import argparse
from datetime import date, datetime, timezone
import json
import os
from pathlib import Path
import re
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT = REPOSITORY_ROOT / "marketplace" / "packages" / "docs" / "data" / "dependencies.json"
SCHEMA = "forkmesh-docs-dependencies-v1"
GITHUB_API = "https://api.github.com"
USER_AGENT = "forkmesh-docs-dependencies"
TOP_AUTHORS = 3
SKIPPED_DIRECTORIES = {
    ".git", ".worktrees", "node_modules", "build", "dist", ".venv", "venv", "__pycache__",
    "tests", "test", "browser_tests_output",
}
PYTHON_MANIFESTS = ["app/host/requirements.txt", "tools/requirements-ci.txt"]
CMAKE_MANIFESTS = ["desktop/CMakeLists.txt"]
GRADLE_ROOT = "native/android"
VENDOR_PARENTS = ["app/public", "www/public", "world/public"]
CMAKE_IGNORED = {"Python3", "PkgConfig", "Threads"}
ECOSYSTEM_ORDER = ["cpp", "python", "go", "npm", "vendored", "maven", "swift"]

KNOWN = {
    ("cpp", "Qt6"): ("Qt 6", "https://github.com/qt/qtbase",
                     "Cross-platform application framework: widgets, networking, SVG and the event loop the desktop runs on."),
    ("cpp", "OpenSSL"): ("OpenSSL", "https://github.com/openssl/openssl",
                         "TLS and general-purpose cryptography library, including the QUIC client."),
    ("cpp", "libnghttp3"): ("nghttp3", "https://github.com/ngtcp2/nghttp3",
                            "HTTP/3 library written in C."),
    ("vendored", "three"): ("three.js", "https://github.com/mrdoob/three.js",
                            "JavaScript 3D library."),
    ("vendored", "satellite-js"): ("satellite.js", "https://github.com/shashwatak/satellite-js",
                                   "Satellite propagation, SGP4/SDP4 orbit calculations in JavaScript."),
    ("vendored", "lucide"): ("Lucide", "https://github.com/lucide-icons/lucide",
                             "Open-source icon library."),
    ("maven", "com.android.application"): ("Android Gradle Plugin", "",
                                           "Builds Android apps with Gradle. Source is hosted on android.googlesource.com, not GitHub."),
    ("maven", "org.jetbrains.kotlin.jvm"): ("Kotlin JVM Gradle plugin", "https://github.com/JetBrains/kotlin",
                                            "The Kotlin compiler and its Gradle plugin."),
    ("maven", "org.jetbrains.kotlin.plugin.compose"): ("Kotlin Compose compiler plugin", "https://github.com/JetBrains/kotlin",
                                                       "The Compose compiler plugin, shipped with Kotlin."),
}
MAVEN_GROUPS = {
    "androidx.": ("https://github.com/androidx/androidx", "Android Jetpack libraries."),
    "org.jetbrains.kotlinx:kotlinx-coroutines": ("https://github.com/Kotlin/kotlinx.coroutines",
                                                 "Library support for Kotlin coroutines."),
}
GITHUB_REPOSITORY = re.compile(r"github\.com[/:]([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+?)(?:\.git)?(?:[/#?].*)?$")


class RateLimited(Exception):
    def __init__(self, reset: int):
        super().__init__(f"GitHub API rate limit reached; it resets at {datetime.fromtimestamp(reset, timezone.utc):%H:%M} UTC")
        self.reset = reset


def github_url(text: str) -> str:
    match = GITHUB_REPOSITORY.search(text.strip()) if text else None
    if not match:
        return ""
    owner, name = match.group(1), match.group(2)
    if owner.lower() in {"sponsors", "orgs", "apps", "features", "marketplace"}:
        return ""
    return f"https://github.com/{owner}/{name}"


def _walk(root: Path, pattern: str):
    for path in sorted(root.rglob(pattern)):
        relative = path.relative_to(root)
        if any(part in SKIPPED_DIRECTORIES or part.startswith(".") for part in relative.parts[:-1]):
            continue
        if path.is_file():
            yield path


def _entry(ecosystem: str, name: str, version: str, source: str, *, development: bool = False,
           title: str = "", description: str = "", repository: str = "") -> dict:
    return {
        "ecosystem": ecosystem,
        "name": name,
        "title": title or name,
        "version": version,
        "development": development,
        "usedBy": [source],
        "description": description,
        "repository": repository,
    }


def python_dependencies(root: Path) -> list[dict]:
    found = []
    pin = re.compile(r"^([A-Za-z0-9][A-Za-z0-9._-]*)(\[[^\]]*\])?\s*(?:==|>=|~=)\s*([^\s;#]+)")
    for manifest in PYTHON_MANIFESTS:
        path = root / manifest
        if not path.is_file():
            continue
        for line in path.read_text(encoding="utf-8").splitlines():
            match = pin.match(line.strip())
            if match:
                found.append(_entry("python", match.group(1), match.group(3), manifest,
                                    development=manifest.startswith("tools/")))
    pyproject = root / "app" / "pyproject.toml"
    if pyproject.is_file():
        block = re.search(r"^dependencies\s*=\s*\[(.*?)\]", pyproject.read_text(encoding="utf-8"), re.S | re.M)
        for spec in re.findall(r'"([^"]+)"', block.group(1) if block else ""):
            match = pin.match(spec)
            name = match.group(1) if match else re.split(r"[<>=!~\[; ]", spec, 1)[0]
            found.append(_entry("python", name, match.group(3) if match else "", "app/pyproject.toml"))
    return found


def npm_dependencies(root: Path) -> list[dict]:
    found = []
    for path in _walk(root, "package.json"):
        try:
            manifest = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        source = path.relative_to(root).as_posix()
        for key, development in (("dependencies", False), ("devDependencies", True)):
            for name, version in sorted((manifest.get(key) or {}).items()):
                found.append(_entry("npm", name, str(version).lstrip("^~="), source, development=development))
    return found


def go_dependencies(root: Path) -> list[dict]:
    found = []
    for path in _walk(root, "go.mod"):
        source = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        lines = []
        for block in re.findall(r"^require\s*\((.*?)^\)", text, re.S | re.M):
            lines += block.splitlines()
        lines += re.findall(r"^require\s+([^(\n].*)$", text, re.M)
        for line in lines:
            parts = line.split("//", 1)[0].split()
            if len(parts) >= 2:
                found.append(_entry("go", parts[0], parts[1].lstrip("v"), source))
    return found


def cmake_dependencies(root: Path) -> list[dict]:
    found = []
    for manifest in CMAKE_MANIFESTS:
        path = root / manifest
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        versions: dict[str, str] = {}
        for name, version in re.findall(r"find_package\(\s*([A-Za-z0-9_]+)\s+([0-9][0-9.]*)?", text):
            if name not in CMAKE_IGNORED:
                versions.setdefault(name, version)
                versions[name] = versions[name] or version
        for name in re.findall(r"pkg_check_modules\([^)]*IMPORTED_TARGET\s+([A-Za-z0-9_.-]+)", text):
            versions.setdefault(name, "")
        for name, version in versions.items():
            title, repository, description = KNOWN.get(("cpp", name), (name, "", ""))
            found.append(_entry("cpp", name, f"{version}+" if version else "", manifest,
                                title=title, description=description, repository=repository))
    return found


def maven_dependencies(root: Path) -> list[dict]:
    found = []
    base = root / GRADLE_ROOT
    if not base.is_dir():
        return found
    for path in sorted(base.rglob("*.gradle.kts")):
        source = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        for plugin, version in re.findall(r'id\("([^"]+)"\)\s*version\s*"([^"]+)"', text):
            title, repository, description = KNOWN.get(("maven", plugin), (plugin, "", ""))
            found.append(_entry("maven", plugin, version, source, development=True,
                                title=title, description=description, repository=repository))
        for coordinate in re.findall(r'(?:implementation|api|platform)\("([^":]+:[^":]+(?::[^"]+)?)"\)', text):
            group, artifact, *rest = coordinate.split(":")
            name = f"{group}:{artifact}"
            repository, description = next(
                (value for prefix, value in MAVEN_GROUPS.items() if name.startswith(prefix)), ("", ""))
            found.append(_entry("maven", name, rest[0] if rest else "", source,
                                description=description, repository=repository))
    return found


def vendored_dependencies(root: Path) -> list[dict]:
    found = []
    named = re.compile(r"^([a-z][a-z0-9-]*?)-(\d+\.\d+\.\d+)(?:[.-]|$)")
    for parent in VENDOR_PARENTS:
        base = root / parent
        if not base.is_dir():
            continue
        for vendor in sorted(path for path in base.rglob("vendor") if path.is_dir()):
            for item in sorted(vendor.iterdir()):
                match = named.match(item.name)
                if not match or (item.is_file() and item.suffix.lower() not in {".js", ".mjs"}):
                    continue
                title, repository, description = KNOWN.get(("vendored", match.group(1)), (match.group(1), "", ""))
                found.append(_entry("vendored", match.group(1), match.group(2),
                                    item.relative_to(root).as_posix(),
                                    title=title, description=description, repository=repository))
    return found


def swift_dependencies(root: Path) -> list[dict]:
    found = []
    for path in _walk(root, "Package.swift"):
        source = path.relative_to(root).as_posix()
        for url, version in re.findall(r'\.package\(\s*url:\s*"([^"]+)"[^)]*?(?:from|exact):\s*"([^"]+)"',
                                       path.read_text(encoding="utf-8")):
            name = url.rstrip("/").removesuffix(".git").rsplit("/", 1)[-1]
            found.append(_entry("swift", name, version, source, repository=github_url(url)))
    return found


def inventory(root: Path = REPOSITORY_ROOT) -> list[dict]:
    merged: dict[tuple[str, str], dict] = {}
    for finder in (cmake_dependencies, python_dependencies, go_dependencies, npm_dependencies,
                   vendored_dependencies, maven_dependencies, swift_dependencies):
        for entry in finder(root):
            key = (entry["ecosystem"], entry["name"].lower())
            if key not in merged:
                merged[key] = entry
                continue
            known = merged[key]
            known["usedBy"] = sorted(set(known["usedBy"]) | set(entry["usedBy"]))
            versions = [value for value in known["version"].split(", ") if value]
            if entry["version"] and entry["version"] not in versions:
                versions.append(entry["version"])
            known["version"] = ", ".join(versions)
            known["development"] = known["development"] and entry["development"]
    for entry in merged.values():
        entry["usedBy"] = sorted(set(entry["usedBy"]))
    return sorted(merged.values(), key=lambda entry: (
        ECOSYSTEM_ORDER.index(entry["ecosystem"]), entry["title"].lower(), entry["name"].lower()))


class Client:
    def __init__(self, token: str = "", wait: bool = False, log=print):
        self.token = token
        self.wait = wait
        self.log = log
        self.calls = 0

    def json(self, url: str, *, github: bool = False):
        headers = {"User-Agent": USER_AGENT, "Accept": "application/json"}
        if github:
            headers["Accept"] = "application/vnd.github+json"
            headers["X-GitHub-Api-Version"] = "2022-11-28"
            if self.token:
                headers["Authorization"] = f"Bearer {self.token}"
        for attempt in range(3):
            try:
                if github:
                    self.calls += 1
                with urlopen(Request(url, headers=headers), timeout=30) as response:
                    body = response.read()
                    return json.loads(body) if body.strip() else None
            except HTTPError as error:
                remaining = error.headers.get("X-RateLimit-Remaining")
                if github and error.code in {403, 429} and remaining == "0":
                    reset = int(error.headers.get("X-RateLimit-Reset", time.time() + 3600))
                    if not self.wait:
                        raise RateLimited(reset) from error
                    pause = max(1, reset - int(time.time()) + 5)
                    self.log(f"rate limited; waiting {pause // 60 + 1} min for the reset")
                    time.sleep(pause)
                    continue
                if error.code in {404, 410, 451}:
                    return None
                if error.code == 403:
                    return {"_forbidden": error.read().decode("utf-8", "replace")}
                if error.code >= 500 and attempt < 2:
                    time.sleep(2 + attempt * 3)
                    continue
                raise
            except URLError:
                if attempt < 2:
                    time.sleep(2 + attempt * 3)
                    continue
                raise
        return None


def registry_details(entry: dict, client: Client) -> tuple[str, str]:
    ecosystem, name = entry["ecosystem"], entry["name"]
    if ecosystem == "python":
        data = client.json(f"https://pypi.org/pypi/{quote(name)}/json") or {}
        info = data.get("info") or {}
        urls = list((info.get("project_urls") or {}).items())
        preferred = [value for key, value in urls if key.lower() in {"source", "source code", "repository", "code", "github"}]
        candidates = preferred + [value for _, value in urls] + [info.get("home_page") or ""]
        return next((url for url in map(github_url, candidates) if url), ""), info.get("summary") or ""
    if ecosystem == "npm":
        data = client.json(f"https://registry.npmjs.org/{quote(name, safe='@')}") or {}
        repository = data.get("repository") or {}
        if isinstance(repository, dict):
            repository = repository.get("url", "")
        candidates = [repository, data.get("homepage") or "", (data.get("bugs") or {}).get("url", "")]
        return next((url for url in map(github_url, candidates) if url), ""), data.get("description") or ""
    if ecosystem == "go":
        if name.startswith("github.com/"):
            return github_url(name), ""
        if name.startswith("golang.org/x/"):
            return f"https://github.com/golang/{name.split('/')[2]}", ""
        if name.startswith("google.golang.org/protobuf"):
            return "https://github.com/protocolbuffers/protobuf-go", ""
    return "", ""


def top_authors(repository: str, client: Client, people: dict[str, dict]) -> tuple[list[dict], str]:
    owner_name = repository.removeprefix("https://github.com/")
    data = client.json(f"{GITHUB_API}/repos/{owner_name}/contributors?per_page=20", github=True)
    if data is None:
        return [], "GitHub returned no contributors for this repository."
    if isinstance(data, dict):
        return [], "GitHub does not list contributors for this repository (its history is too large)."
    authors = []
    for contributor in data:
        login = contributor.get("login") or ""
        if contributor.get("type") != "User" or not login or login.endswith("[bot]"):
            continue
        person = people.get(login)
        if person is None:
            profile = client.json(f"{GITHUB_API}/users/{quote(login)}", github=True) or {}
            person = {
                "login": login,
                "name": (profile.get("name") or "").strip() or login,
                "profile": profile.get("html_url") or f"https://github.com/{login}",
                "projects": f"https://github.com/{login}?tab=repositories",
                "publicRepos": int(profile.get("public_repos") or 0),
                "website": _website(profile.get("blog") or ""),
            }
            people[login] = person
        authors.append(person | {"contributions": int(contributor.get("contributions") or 0)})
        if len(authors) == TOP_AUTHORS:
            break
    return authors, "" if authors else "GitHub lists no human contributors for this repository."


def _website(value: str) -> str:
    value = value.strip()
    if not value:
        return ""
    if not re.match(r"^https?://", value, re.I):
        value = "https://" + value
    return value if re.match(r"^https?://[^\s/]+\.[^\s]+$", value, re.I) else ""


def _stale(fetched: str, max_age: int) -> bool:
    try:
        return (date.today() - date.fromisoformat(fetched)).days > max_age
    except ValueError:
        return True


def build(root: Path, previous: dict | None, client: Client | None, *, refresh: bool = False,
          max_age: int = 30, log=print, checkpoint=None) -> tuple[dict, str]:
    entries = inventory(root)
    cached = {(item["ecosystem"], item["name"].lower()): item
              for item in (previous or {}).get("dependencies", [])}
    people: dict[str, dict] = {}
    if not refresh:
        for item in cached.values():
            for author in item.get("authors", []):
                people.setdefault(author["login"], {k: v for k, v in author.items() if k != "contributions"})
    for entry in entries:
        old = cached.get((entry["ecosystem"], entry["name"].lower()), {})
        for key in ("repository", "description"):
            if not entry[key] and old.get(key):
                entry[key] = old[key]
        entry["authors"] = old.get("authors", [])
        entry["authorsNote"] = old.get("authorsNote", "")
        entry["fetched"] = old.get("fetched", "")
    snapshot = {
        "schema": SCHEMA,
        "generated": date.today().isoformat(),
        "generator": "marketplace/tools/docs_dependencies.py",
        "dependencies": entries,
    }
    if client is None:
        return snapshot, ""
    for entry in entries:
        old = cached.get((entry["ecosystem"], entry["name"].lower()), {})
        try:
            if not entry["repository"] or (refresh and entry["ecosystem"] in {"python", "npm", "go"}):
                repository, description = registry_details(entry, client)
                entry["repository"] = repository or entry["repository"]
                entry["description"] = entry["description"] or description.strip()
            if not entry["repository"]:
                entry["authors"], entry["authorsNote"] = [], "No GitHub repository is known for this dependency."
                entry["fetched"] = date.today().isoformat()
            elif refresh or not entry["fetched"] or _stale(entry["fetched"], max_age) or (
                    old.get("repository") and old.get("repository") != entry["repository"]):
                log(f"{entry['ecosystem']:8} {entry['name']}: {entry['repository']}")
                entry["authors"], entry["authorsNote"] = top_authors(entry["repository"], client, people)
                entry["fetched"] = date.today().isoformat()
            else:
                continue
        except RateLimited as error:
            return snapshot, str(error)
        if checkpoint:
            checkpoint(snapshot)
    return snapshot, ""


def drift(snapshot: dict, root: Path = REPOSITORY_ROOT) -> list[str]:
    def keyed(items):
        return {(item["ecosystem"], item["name"]): (item["version"], tuple(item["usedBy"])) for item in items}
    expected, recorded = keyed(inventory(root)), keyed(snapshot.get("dependencies", []))
    problems = [f"missing {ecosystem} {name}" for ecosystem, name in sorted(expected.keys() - recorded.keys())]
    problems += [f"no longer used: {ecosystem} {name}" for ecosystem, name in sorted(recorded.keys() - expected.keys())]
    problems += [f"changed {ecosystem} {name}: {recorded[(ecosystem, name)]} -> {expected[(ecosystem, name)]}"
                 for ecosystem, name in sorted(expected.keys() & recorded.keys())
                 if expected[(ecosystem, name)] != recorded[(ecosystem, name)]]
    return problems


def write_snapshot(path: Path, snapshot: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(snapshot, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Inventory ForkMesh's open-source dependencies and record each one's GitHub "
                    "repository and top three authors for the Docs package.")
    parser.add_argument("--output", type=Path, default=SNAPSHOT, help="snapshot to read and write")
    parser.add_argument("--offline", action="store_true",
                        help="re-read the manifests only; keep the GitHub data already in the snapshot")
    parser.add_argument("--refresh", action="store_true", help="fetch every repository and author again")
    parser.add_argument("--max-age", type=int, default=30,
                        help="refetch authors recorded more than this many days ago (default 30)")
    parser.add_argument("--wait", action="store_true",
                        help="sleep through GitHub rate limits instead of stopping")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if the snapshot does not match the manifests; write nothing")
    args = parser.parse_args(argv)
    sys.stdout.reconfigure(line_buffering=True)
    previous = json.loads(args.output.read_text(encoding="utf-8")) if args.output.is_file() else None
    if args.check:
        problems = drift(previous or {})
        for problem in problems:
            print(problem)
        return 1 if problems else 0
    client = None if args.offline else Client(os.environ.get("GITHUB_TOKEN", ""), wait=args.wait)
    snapshot, stopped = build(REPOSITORY_ROOT, previous, client, refresh=args.refresh, max_age=args.max_age,
                              checkpoint=lambda partial: write_snapshot(args.output, partial))
    write_snapshot(args.output, snapshot)
    total = len(snapshot["dependencies"])
    with_authors = sum(1 for entry in snapshot["dependencies"] if entry["authors"])
    print(f"{total} dependencies, {with_authors} with authors, {client.calls if client else 0} GitHub calls")
    if stopped:
        print(f"stopped early: {stopped}; run again to continue", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
