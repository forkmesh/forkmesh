#!/usr/bin/env python3
"""Exercise stock dependency managers against provider-independent routes.

Implements the forkmesh-dependency-workflows-v1 contract
(conformance/dependencies/v1/contract.json, www/docs/dependency-workflows.md).
Disposable loopback providers serve one fixture repository. Every install runs
an unmodified client (Git, pip, npm, Go, Cargo) against one logical URL, and
Git's ``url.<base>.insteadOf`` points that URL at whichever provider is active.
A verifying relay uses app/src/stable_repository.py to fail over, in declared
order, only to a route serving the state the source signed.

  dependency_conformance.py validate
  dependency_conformance.py run [--clients git,pip] [--require git,pip]
                                [--client-label LABEL] [--output report.json]

Exit codes: 0 every scenario matched the contract, 1 an unexpected result, a
missing required client, or an invalid contract, 2 the probe could not run.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT / "conformance" / "dependencies" / "v1" / "contract.json"
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(ROOT / "tools"))

import forkmesh_safe_delete  # noqa: E402

import stable_repository  # noqa: E402
from git_http import advertised_refs_canonical  # noqa: E402
from urls import GIT_INFO_RE, GIT_PACK_RE  # noqa: E402


def _load_git_conformance():
    spec = importlib.util.spec_from_file_location(
        "forkmesh_git_conformance", ROOT / "tools" / "git_conformance.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


stock_git = _load_git_conformance()

REPORT_SCHEMA = "forkmesh-dependency-conformance-report-v1"
CLIENTS = ("git", "git-submodule", "pip", "npm", "go", "cargo")
SCENARIOS = (
    "install",
    "provider-switch",
    "origin-lost",
    "unverified-fallback",
    "missing-commit",
    "moved-tag",
    "rollback-recovery",
)
# Scenarios whose installs must reproduce the "install" lock and content.
REPRODUCED = ("provider-switch", "origin-lost", "rollback-recovery")
LOGICAL_HOST = "deps.forkmesh.test"
LOGICAL_BASE = "https://" + LOGICAL_HOST
OWNER = "acme"
REPOSITORY = "fmprobe"
LOGICAL_URL = "%s/%s/%s.git" % (LOGICAL_BASE, OWNER, REPOSITORY)
GO_MODULE = "%s/%s/%s.git" % (LOGICAL_HOST, OWNER, REPOSITORY)
TAG = "v1.0.0"
MISSING_COMMIT = hashlib.sha1(b"forkmesh dependency conformance: absent").hexdigest()
FIXED_DATE = "2001-01-01T00:00:00+00:00"
COMMAND_TIMEOUT = 300
FORWARDED_REQUEST_HEADERS = {"content-type", "content-encoding", "git-protocol", "user-agent", "accept"}
DROPPED_RESPONSE_HEADERS = {"connection", "transfer-encoding", "server", "date", "content-length"}


class ConformanceError(RuntimeError):
    """The probe cannot run; distinct from a scenario that did not match."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# --- Contract ----------------------------------------------------------------


def load_contract() -> dict:
    return json.loads(CONTRACT_PATH.read_text(encoding="utf-8"))


def validate_contract(contract: dict) -> list[str]:
    """Return every way the contract, reference, docs, and CI disagree."""
    problems = []
    if contract.get("schema") != "forkmesh-dependency-workflows-contract-v1":
        problems.append("unexpected contract schema")
    if contract.get("reportSchema") != REPORT_SCHEMA:
        problems.append("reportSchema does not match the probe")
    if contract.get("resolutionSchema") != stable_repository.RESOLUTION_SCHEMA:
        problems.append("resolutionSchema does not match the reference")
    if tuple(item.get("id") for item in contract.get("clients", ())) != CLIENTS:
        problems.append("client ids do not match the probe")
    if tuple(item.get("id") for item in contract.get("scenarios", ())) != SCENARIOS:
        problems.append("scenario ids do not match the probe")
    if tuple(contract.get("routeReasons", ())) != stable_repository.ROUTE_REASONS:
        problems.append("routeReasons do not match the reference")
    if tuple(contract.get("pinErrors", ())) != stable_repository.PIN_ERRORS:
        problems.append("pinErrors do not match the reference")
    if tuple(contract.get("resolutionStatuses", ())) != stable_repository.STATUSES:
        problems.append("resolutionStatuses do not match the reference")
    reasons = set(stable_repository.ROUTE_REASONS)
    for scenario in contract.get("scenarios", ()):
        if scenario.get("expect") not in {"installed", "refused"}:
            problems.append("%s: expect must be installed or refused" % scenario.get("id"))
        if scenario.get("reason") and scenario["reason"] not in reasons:
            problems.append("%s: unknown reason" % scenario.get("id"))
    for field in ("documentation", "referenceImplementation", "probe", "test", "workflow"):
        path = contract.get(field)
        if not isinstance(path, str) or not (ROOT / path).is_file():
            problems.append("%s does not name a repository file" % field)
    documentation = ROOT / str(contract.get("documentation", ""))
    if documentation.is_file():
        text = documentation.read_text(encoding="utf-8")
        tokens = (
            [item["id"] for item in contract.get("scenarios", ())]
            + list(contract.get("routeReasons", ()))
            + list(contract.get("pinErrors", ()))
            + list(contract.get("resolutionStatuses", ()))
            + list(contract.get("telemetry", {}).get("summary", ()))
        )
        for token in tokens:
            if "`%s`" % token not in text:
                problems.append("documentation does not describe `%s`" % token)
        for client in contract.get("clients", ()):
            if "### %s" % client.get("name") not in text:
                problems.append("documentation has no recipe for %s" % client.get("name"))
    workflow = ROOT / str(contract.get("workflow", ""))
    if workflow.is_file():
        text = workflow.read_text(encoding="utf-8")
        for marker in ("tools/dependency_conformance.py run", "--require", *contract.get("platforms", ())):
            if marker not in text:
                problems.append("workflow does not run %s" % marker)
    return problems


# --- Loopback providers --------------------------------------------------------


def _remove_tree(path: Path) -> None:
    # Go writes its module cache read-only; clear the bits before deleting.
    if not path.exists():
        return
    forkmesh_safe_delete.check(path, path.parent)
    for directory, subdirectories, files in os.walk(path):
        for name in subdirectories + files:
            try:
                os.chmod(os.path.join(directory, name), stat.S_IWRITE | stat.S_IREAD | stat.S_IEXEC)
            except OSError:
                pass
    forkmesh_safe_delete.remove_tree(path, path.parent, ignore_errors=True)


class VerifyingRelay:
    """Serve one stable URL from the first declared route with a signed state.

    Before any response byte, each route's protocol-v0 advertisement is read
    and canonicalized exactly as the public relay does (FM-ID-005); the
    reference ``select_route`` then picks a route or refuses.
    """

    def __init__(self, accepted_states: set[str]) -> None:
        self.accepted_states = set(accepted_states)
        self.routes: list[dict] = []
        self.reports: list[dict] = []
        self.identity = ""

    def configure(self, identity: str, routes: list[dict]) -> None:
        self.identity = identity
        self.routes = list(routes)
        self.reports = []

    def _observe(self, route: dict, owner: str, repository: str) -> dict:
        observation = {"route": route["route"], "reachable": False,
                       "identity": route["identity"], "stateHash": ""}
        parsed = urlsplit(route["base"])
        connection = HTTPConnection(parsed.hostname, parsed.port, timeout=30)
        try:
            connection.request(
                "GET", "/%s/%s.git/info/refs?service=git-upload-pack" % (owner, repository))
            response = connection.getresponse()
            body = response.read()
            content_type = response.getheader("Content-Type") or ""
            if response.status == 200 and "git-upload-pack-advertisement" in content_type:
                observation["reachable"] = True
                observation["stateHash"] = _sha256(
                    advertised_refs_canonical(body).encode("utf-8"))
        except OSError:
            pass
        finally:
            connection.close()
        return observation

    def handle(self, method: str, target: str, headers: dict[str, str], body: bytes):
        parsed = urlsplit(target)
        match = GIT_INFO_RE.match(parsed.path) or GIT_PACK_RE.match(parsed.path)
        if not match:
            return 404, {"Content-Type": "text/plain"}, b"not found\n"
        owner, repository = match.group(1), match.group(2)
        observations = [self._observe(route, owner, repository) for route in self.routes]
        report = stable_repository.select_route(
            self.identity, observations, self.accepted_states)
        self.reports.append(report)
        if report["selected"] is None:
            return 503, {"Content-Type": "text/plain"}, b"no verified route\n"
        route = next(item for item in self.routes if item["route"] == report["selected"])
        suffix = "info/refs" if GIT_INFO_RE.match(parsed.path) else "git-upload-pack"
        upstream_path = "/%s/%s.git/%s" % (owner, repository, suffix)
        if parsed.query:
            upstream_path += "?" + parsed.query
        base = urlsplit(route["base"])
        connection = HTTPConnection(base.hostname, base.port, timeout=60)
        try:
            forwarded = {name: value for name, value in headers.items()
                         if name.lower() in FORWARDED_REQUEST_HEADERS}
            connection.request(method, upstream_path, body=body, headers=forwarded)
            response = connection.getresponse()
            payload = response.read()
            passed = {name: value for name, value in response.getheaders()
                      if name.lower() not in DROPPED_RESPONSE_HEADERS}
            return response.status, passed, payload
        except OSError:
            return 503, {"Content-Type": "text/plain"}, b"route failed after selection\n"
        finally:
            connection.close()


class Fixture:
    """One repository, served as origin, faithful mirror, tampered, and retagged copies."""

    def __init__(self, root: Path, git) -> None:
        self.root = root
        self.git = git
        self.providers = root / "providers"
        self.commit = ""
        self.head = ""
        self.identity = ""
        self.states: dict[str, str] = {}
        self.superprojects: dict[str, Path] = {}

    def repository(self, provider: str) -> Path:
        return self.providers / provider / OWNER / (REPOSITORY + ".git")

    def _commit(self, work: Path, files: dict[str, str], message: str) -> str:
        for relative, content in files.items():
            target = work / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(content.encode("utf-8"))
        self.git.run(["-C", str(work), "add", "-A"])
        self.git.run(["-C", str(work), "commit", "-q", "-m", message],
                     extra_environment={"GIT_AUTHOR_DATE": FIXED_DATE, "GIT_COMMITTER_DATE": FIXED_DATE})
        return self.git.oid(work)

    def create(self) -> None:
        work = self.root / "source"
        self.git.run(["init", "-q", "-b", "main", str(work)])
        stock_git._configure_identity(self.git, work)
        self.commit = self._commit(work, _package_files("pinned"), "pinned release")
        self.git.run(["-C", str(work), "tag", TAG, self.commit])
        self.head = self._commit(work, _package_files("next"), "later work")
        origin = self.repository("origin")
        origin.parent.mkdir(parents=True)
        self.git.run(["clone", "-q", "--bare", str(work), str(origin)])
        for provider in ("mirror", "tampered", "retagged"):
            destination = self.repository(provider)
            destination.parent.mkdir(parents=True)
            self.git.run(["clone", "-q", "--bare", str(origin), str(destination)])
        # A tampered copy rewrites main; a retagged copy moves the release tag.
        tamper = self.root / "tamper"
        self.git.run(["clone", "-q", str(origin), str(tamper)])
        stock_git._configure_identity(self.git, tamper)
        tampered = self._commit(tamper, _package_files("tampered"), "unsigned change")
        self.git.run(["--git-dir", str(self.repository("tampered")), "fetch", "-q",
                      str(tamper), "+refs/heads/main:refs/heads/main"])
        self.git.run(["--git-dir", str(self.repository("retagged")), "update-ref",
                      "refs/tags/" + TAG, self.head])
        if tampered == self.head:
            raise ConformanceError("tampered fixture did not diverge")
        for provider in ("origin", "mirror", "tampered", "retagged"):
            self.states[provider] = self.state_hash(provider)
        root_commit = self.git.text([
            "--git-dir", str(origin), "rev-list", "--max-parents=0", "refs/heads/main"])
        self.identity = stable_repository.repository_identity(root_commit)
        for name, gitlink in (("pinned", self.commit), ("missing", MISSING_COMMIT)):
            superproject = self.root / ("superproject-" + name)
            self.git.run(["init", "-q", "-b", "main", str(superproject)])
            stock_git._configure_identity(self.git, superproject)
            (superproject / ".gitmodules").write_text(
                '[submodule "fmprobe"]\n\tpath = deps/fmprobe\n\turl = %s\n' % LOGICAL_URL,
                encoding="utf-8")
            self.git.run(["-C", str(superproject), "add", ".gitmodules"])
            self.git.run(["-C", str(superproject), "update-index", "--add", "--cacheinfo",
                          "160000,%s,deps/fmprobe" % gitlink])
            self.git.run(["-C", str(superproject), "commit", "-q", "-m", "pin dependency"],
                         extra_environment={"GIT_AUTHOR_DATE": FIXED_DATE, "GIT_COMMITTER_DATE": FIXED_DATE})
            self.superprojects[name] = superproject

    def refs(self, provider: str) -> dict[str, str]:
        output = self.git.text([
            "--git-dir", str(self.repository(provider)), "for-each-ref", "--sort=refname",
            "--format=%(objectname) %(refname)", "refs/heads", "refs/tags"])
        refs = {}
        for line in output.splitlines():
            oid, name = line.split(" ", 1)
            refs[name] = oid
        return refs

    def state_hash(self, provider: str) -> str:
        # The source-side canonicalization the relay must agree with (FM-ID-005).
        lines = ["%s %s" % (oid, name) for name, oid in self.refs(provider).items()]
        return _sha256("\n".join(lines).encode("utf-8"))

    def has_commit(self, provider: str, commit: str) -> bool:
        completed = self.git.run(
            ["--git-dir", str(self.repository(provider)), "cat-file", "-e", commit + "^{commit}"],
            check=False)
        return completed.returncode == 0


def _package_files(value: str) -> dict[str, str]:
    return {
        "fmprobe/__init__.py": 'VALUE = "%s"\n' % value,
        "pyproject.toml": (
            '[build-system]\nrequires = []\nbuild-backend = "fmprobe_build"\n'
            'backend-path = ["."]\n\n[project]\nname = "fmprobe"\nversion = "1.0.0"\n'),
        "fmprobe_build.py": _BUILD_BACKEND,
        "package.json": '{"name": "fmprobe", "version": "1.0.0", "main": "index.js"}\n',
        "index.js": 'module.exports = "%s";\n' % value,
        "go.mod": "module %s\n\ngo 1.21\n" % GO_MODULE,
        "fmprobe.go": 'package fmprobe\n\nconst Value = "%s"\n' % value,
        "Cargo.toml": '[package]\nname = "fmprobe"\nversion = "1.0.0"\nedition = "2021"\n',
        "src/lib.rs": 'pub const VALUE: &str = "%s";\n' % value,
    }


# A stdlib-only PEP 517 backend, so pip needs no build dependency or index.
_BUILD_BACKEND = '''import base64
import hashlib
import os
import zipfile

NAME = "fmprobe"
VERSION = "1.0.0"


def get_requires_for_build_wheel(config_settings=None):
    return []


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    info = "%s-%s.dist-info" % (NAME, VERSION)
    with open(os.path.join("fmprobe", "__init__.py"), "rb") as source:
        module = source.read()
    files = [
        ("fmprobe/__init__.py", module),
        (info + "/METADATA", ("Metadata-Version: 2.1\\nName: %s\\nVersion: %s\\n" % (NAME, VERSION)).encode()),
        (info + "/WHEEL", b"Wheel-Version: 1.0\\nGenerator: fmprobe_build\\nRoot-Is-Purelib: true\\nTag: py3-none-any\\n"),
    ]
    wheel = "%s-%s-py3-none-any.whl" % (NAME, VERSION)
    record = []
    with zipfile.ZipFile(os.path.join(wheel_directory, wheel), "w") as archive:
        for path, data in files:
            archive.writestr(path, data)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            record.append("%s,sha256=%s,%d" % (path, digest, len(data)))
        record.append(info + "/RECORD,,")
        archive.writestr(info + "/RECORD", "\\n".join(record) + "\\n")
    return wheel
'''


# --- Stock clients -------------------------------------------------------------


def _client_executables() -> dict[str, list[str] | None]:
    git = shutil.which("git")
    npm = shutil.which("npm")
    go = shutil.which("go")
    cargo = shutil.which("cargo")
    return {
        "git": [git] if git else None,
        "git-submodule": [git] if git else None,
        "pip": [sys.executable, "-m", "pip"],
        "npm": [npm] if npm else None,
        "go": [go] if go else None,
        "cargo": [cargo] if cargo else None,
    }


def _version(command: list[str] | None, client: str) -> str:
    if not command:
        return ""
    arguments = ["version"] if client == "go" else ["--version"]
    try:
        completed = subprocess.run(command + arguments, capture_output=True, timeout=60, check=False)
    except (OSError, subprocess.SubprocessError):
        return ""
    if completed.returncode:
        return ""
    match = re.search(r"(\d+\.\d+(?:\.\d+)?)", completed.stdout.decode("utf-8", "replace"))
    return match.group(1) if match else ""


def _version_tuple(value: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r"\d+", value)[:3])


class Workspace:
    """Isolated caches and Git configuration for one client in one scenario."""

    def __init__(self, root: Path, active_base: str) -> None:
        self.root = root
        self.root.mkdir(parents=True)
        gitconfig = root / "gitconfig"
        gitconfig.write_text(
            '[url "%s/"]\n\tinsteadOf = %s/\n[advice]\n\tdetachedHead = false\n'
            % (active_base, LOGICAL_BASE),
            encoding="utf-8")
        (root / "npmrc").write_text("", encoding="utf-8")
        rustup_home = os.environ.get("RUSTUP_HOME") or str(Path.home() / ".rustup")
        environment = {
            key: value for key, value in os.environ.items()
            if not key.upper().startswith(("GIT_", "NPM_CONFIG_", "GO", "CARGO_", "PIP_"))
        }
        environment.update({
            "GIT_CONFIG_GLOBAL": str(gitconfig),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0",
            "GIT_ALLOW_PROTOCOL": "file:http:https",
            "npm_config_userconfig": str(root / "npmrc"),
            "npm_config_cache": str(root / "npm-cache"),
            "npm_config_audit": "false",
            "npm_config_fund": "false",
            "npm_config_update_notifier": "false",
            "npm_config_ignore_scripts": "true",
            "GOPATH": str(root / "gopath"),
            "GOMODCACHE": str(root / "gopath" / "pkg" / "mod"),
            "GOCACHE": str(root / "gocache"),
            "GOPROXY": "direct",
            "GOPRIVATE": LOGICAL_HOST,
            "GOFLAGS": "-mod=mod",
            "GOTOOLCHAIN": "local",
            "GOTELEMETRY": "off",
            "GOENV": "off",
            "GOWORK": "off",
            "CARGO_HOME": str(root / "cargo-home"),
            "CARGO_TARGET_DIR": str(root / "cargo-target"),
            "CARGO_NET_GIT_FETCH_WITH_CLI": "true",
            "CARGO_TERM_COLOR": "never",
            "PIP_CONFIG_FILE": os.devnull,
            "PIP_DISABLE_PIP_VERSION_CHECK": "1",
            "PIP_NO_INPUT": "1",
        })
        if Path(rustup_home).is_dir():
            environment["RUSTUP_HOME"] = rustup_home
        self.environment = environment

    def run(self, command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
        try:
            return subprocess.run(
                command, cwd=cwd or self.root, env=self.environment, capture_output=True,
                timeout=COMMAND_TIMEOUT, check=False)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(command, 124, b"", b"timed out")
        except OSError as exc:
            return subprocess.CompletedProcess(command, 127, b"", str(exc).encode())


class Install:
    """One client's attempt: exit status, resolved commit, lock evidence, and content."""

    def __init__(self, returncode: int, commit: str = "", lock: str = "",
                 content: bytes | None = None, output: bytes = b"") -> None:
        self.returncode = returncode
        self.commit = commit
        self.lock = lock
        self.content = content
        self.output = output


def _first_failure(*completed: subprocess.CompletedProcess[bytes]) -> Install | None:
    for item in completed:
        if item.returncode:
            return Install(item.returncode, output=item.stdout + item.stderr)
    return None


def install_git(space: Workspace, command: list[str], pin: str, _fixture: Fixture) -> Install:
    destination = space.root / "dependency"
    steps = [space.run(command + ["clone", "-q", LOGICAL_URL, str(destination)])]
    if not steps[0].returncode:
        steps.append(space.run(command + ["-C", str(destination), "checkout", "-q", "--detach", pin]))
    failure = _first_failure(*steps)
    if failure:
        return failure
    head = space.run(command + ["-C", str(destination), "rev-parse", "HEAD"]).stdout.decode().strip()
    return Install(0, head, head, (destination / "fmprobe" / "__init__.py").read_bytes())


def install_git_submodule(space: Workspace, command: list[str], pin: str, fixture: Fixture) -> Install:
    superproject = fixture.superprojects["pinned" if pin == fixture.commit else "missing"]
    destination = space.root / "consumer"
    completed = space.run(command + ["clone", "-q", "--recurse-submodules", str(superproject), str(destination)])
    if completed.returncode:
        return Install(completed.returncode, output=completed.stdout + completed.stderr)
    child = destination / "deps" / "fmprobe"
    head = space.run(command + ["-C", str(child), "rev-parse", "HEAD"]).stdout.decode().strip()
    gitlink = space.run(command + ["-C", str(destination), "ls-tree", "HEAD", "deps/fmprobe"]).stdout.decode().strip()
    content_path = child / "fmprobe" / "__init__.py"
    return Install(0, head, gitlink, content_path.read_bytes() if content_path.is_file() else None)


def install_pip(space: Workspace, command: list[str], pin: str, _fixture: Fixture) -> Install:
    target = space.root / "site-packages"
    completed = space.run(command + [
        "install", "--no-index", "--no-build-isolation", "--no-deps", "--no-cache-dir",
        "--target", str(target), "fmprobe @ git+%s@%s" % (LOGICAL_URL, pin)])
    if completed.returncode:
        return Install(completed.returncode, output=completed.stdout + completed.stderr)
    direct_url = json.loads((target / "fmprobe-1.0.0.dist-info" / "direct_url.json").read_text(encoding="utf-8"))
    commit = str(direct_url.get("vcs_info", {}).get("commit_id") or "")
    lock = "fmprobe @ git+%s@%s" % (direct_url.get("url"), commit)
    return Install(0, commit, lock, (target / "fmprobe" / "__init__.py").read_bytes())


def install_npm(space: Workspace, command: list[str], pin: str, _fixture: Fixture) -> Install:
    project = space.root / "consumer"
    project.mkdir()
    (project / "package.json").write_text(
        '{"name": "consumer", "version": "1.0.0", "private": true}\n', encoding="utf-8")
    completed = space.run(command + ["install", "git+%s#%s" % (LOGICAL_URL, pin)], cwd=project)
    if completed.returncode:
        return Install(completed.returncode, output=completed.stdout + completed.stderr)
    lock = json.loads((project / "package-lock.json").read_text(encoding="utf-8"))
    resolved = str(lock.get("packages", {}).get("node_modules/fmprobe", {}).get("resolved") or "")
    commit = resolved.rsplit("#", 1)[-1] if "#" in resolved else ""
    content = project / "node_modules" / "fmprobe" / "index.js"
    return Install(0, commit, resolved, content.read_bytes() if content.is_file() else None)


def _go_download(space: Workspace, command: list[str], project: Path) -> tuple[subprocess.CompletedProcess[bytes], dict]:
    completed = space.run(command + ["mod", "download", "-json", GO_MODULE], cwd=project)
    try:
        details = json.loads(completed.stdout.decode("utf-8", "replace") or "{}")
    except ValueError:
        details = {}
    return completed, details


def _go_install(space: Workspace, command: list[str], version: str) -> tuple[Install, Path]:
    project = space.root / "consumer"
    project.mkdir()
    (project / "go.mod").write_text("module consumer\n\ngo 1.21\n", encoding="utf-8")
    got = space.run(command + ["get", "%s@%s" % (GO_MODULE, version)], cwd=project)
    if got.returncode:
        return Install(got.returncode, output=got.stdout + got.stderr), project
    downloaded, details = _go_download(space, command, project)
    if downloaded.returncode or details.get("Error"):
        return Install(downloaded.returncode or 1, output=downloaded.stdout + downloaded.stderr), project
    origin = details.get("Origin")
    if not origin and details.get("Info"):
        # A cache hit omits Origin from -json; the cached .info file keeps it.
        try:
            origin = json.loads(Path(str(details["Info"])).read_text(encoding="utf-8")).get("Origin")
        except (OSError, ValueError):
            origin = None
    commit = str((origin or {}).get("Hash") or "")
    go_sum = (project / "go.sum").read_text(encoding="utf-8")
    lock = "\n".join(sorted(line for line in go_sum.splitlines() if line.startswith(GO_MODULE + " ")))
    source = Path(str(details.get("Dir") or "")) / "fmprobe.go"
    return Install(0, commit, lock, source.read_bytes() if source.is_file() else None), project


def install_go(space: Workspace, command: list[str], pin: str, _fixture: Fixture) -> Install:
    return _go_install(space, command, pin)[0]


def install_cargo(space: Workspace, command: list[str], pin: str, _fixture: Fixture) -> Install:
    project = space.root / "consumer"
    (project / "src").mkdir(parents=True)
    (project / "src" / "lib.rs").write_text("", encoding="utf-8")
    (project / "Cargo.toml").write_text(
        '[package]\nname = "consumer"\nversion = "0.1.0"\nedition = "2021"\n\n'
        '[dependencies]\nfmprobe = { git = "%s", rev = "%s" }\n' % (LOGICAL_URL, pin),
        encoding="utf-8")
    completed = space.run(command + ["fetch"], cwd=project)
    if completed.returncode:
        return Install(completed.returncode, output=completed.stdout + completed.stderr)
    lock_text = (project / "Cargo.lock").read_text(encoding="utf-8")
    source = next((line.strip() for line in lock_text.splitlines()
                   if line.strip().startswith('source = "git+%s' % LOGICAL_URL)), "")
    commit = source.rstrip('"').rsplit("#", 1)[-1] if "#" in source else ""
    checkouts = sorted((space.root / "cargo-home" / "git" / "checkouts").glob("fmprobe-*/*/src/lib.rs"))
    return Install(0, commit, source, checkouts[0].read_bytes() if checkouts else None)


INSTALLERS = {
    "git": install_git,
    "git-submodule": install_git_submodule,
    "pip": install_pip,
    "npm": install_npm,
    "go": install_go,
    "cargo": install_cargo,
}


# --- Suite -----------------------------------------------------------------------


class Suite:
    def __init__(self, root: Path, contract: dict, clients: list[str], executables: dict) -> None:
        self.root = root
        self.contract = contract
        self.clients = clients
        self.executables = executables
        self.expectations = {item["id"]: item for item in contract["scenarios"]}
        git = executables["git"]
        if not git:
            raise ConformanceError("git is required to build the fixture")
        self.git = stock_git.GitRunner(git[0], "2")
        self.fixture = Fixture(root, self.git)
        self.results: list[dict] = []
        self.resolutions: list[dict] = []
        self.baseline: dict[str, dict] = {}

    def _route(self, label: str, base: str) -> dict:
        return {"route": label, "base": base, "identity": self.fixture.identity}

    def _record_relay(self, scenario: str, relay: VerifyingRelay) -> None:
        grouped: dict[str, dict] = {}
        for report in relay.reports:
            key = json.dumps([report["status"], report["selected"], report["attempts"]], sort_keys=True)
            if key in grouped:
                grouped[key]["requests"] += 1
            else:
                grouped[key] = {"scenario": scenario, "source": "relay", "requests": 1,
                                "status": report["status"], "selected": report["selected"],
                                "fallback": report["fallback"], "errors": report["errors"],
                                "attempts": report["attempts"]}
        self.resolutions.extend(grouped.values())

    def _check(self, scenario: str, client: str, install: Install, pin: str, elapsed_ms: int) -> None:
        expected = self.expectations[scenario]["expect"]
        if install.returncode:
            outcome = "refused"
        elif install.commit != pin:
            outcome = "substituted"
        else:
            outcome = "installed"
        result = {
            "scenario": scenario,
            "client": client,
            "expected": expected,
            "outcome": outcome,
            "ok": outcome == expected,
            "reason": None,
            "lockDigest": _sha256(install.lock.encode("utf-8")) if outcome == "installed" else None,
            "contentDigest": _sha256(install.content) if outcome == "installed" and install.content is not None else None,
            "durationMs": elapsed_ms,
        }
        if outcome == "installed" and scenario == "install":
            self.baseline[client] = result
        if outcome == "installed" and scenario in REPRODUCED:
            baseline = self.baseline.get(client)
            if not baseline:
                result["ok"], result["reason"] = False, "no_install_baseline"
            elif (baseline["lockDigest"], baseline["contentDigest"]) != (result["lockDigest"], result["contentDigest"]):
                result["ok"], result["reason"] = False, "lock_changed"
        if not result["ok"] and result["reason"] is None:
            result["reason"] = "unexpected_" + outcome
        self.results.append(result)

    def _run_clients(self, scenario: str, active_base: str, pin: str, clients: list[str]) -> None:
        for client in clients:
            space = Workspace(self.root / "work" / scenario / client, active_base)
            started = time.monotonic()
            try:
                install = INSTALLERS[client](space, self.executables[client], pin, self.fixture)
            except (OSError, ValueError, KeyError) as exc:
                install = Install(1, output=str(exc).encode())
            self._check(scenario, client, install, pin, int((time.monotonic() - started) * 1000))
            _remove_tree(space.root)

    def _moved_tag(self, origin: str, retagged: str) -> None:
        pin = {"identity": self.fixture.identity, "commit": self.fixture.commit, "ref": "refs/tags/" + TAG}
        observation = {"route": "retagged", "reachable": True, "identity": self.fixture.identity,
                       "stateHash": self.fixture.states["retagged"],
                       "hasCommit": self.fixture.has_commit("retagged", self.fixture.commit),
                       "refs": self.fixture.refs("retagged")}
        # The source re-signed after moving the tag: the state is valid, the pin is not.
        report = stable_repository.resolve(
            pin, [observation], {self.fixture.states["origin"], self.fixture.states["retagged"]})
        self.resolutions.append({"scenario": "moved-tag", "source": "reference", "requests": 1,
                                 "status": report["status"], "selected": report["selected"],
                                 "fallback": report["fallback"], "errors": report["errors"],
                                 "attempts": report["attempts"]})
        if "go" not in self.clients:
            return
        started = time.monotonic()
        locked = Workspace(self.root / "work" / "moved-tag" / "go-lock", origin)
        first, project = _go_install(locked, self.executables["go"], TAG)
        if first.returncode or first.commit != self.fixture.commit:
            install = Install(0, first.commit, first.lock, first.content) if not first.returncode else first
            self._check("moved-tag", "go", Install(0, "setup-failed"), self.fixture.commit,
                        int((time.monotonic() - started) * 1000))
            self.results[-1]["reason"] = "go_tag_lock_setup_failed"
            _remove_tree(locked.root)
            return
        moved = Workspace(self.root / "work" / "moved-tag" / "go-verify", retagged)
        verify = moved.run(self.executables["go"] + ["mod", "download", "-json", GO_MODULE], cwd=project)
        text = (verify.stdout + verify.stderr).decode("utf-8", "replace")
        detected = verify.returncode != 0 or '"Error"' in text
        install = Install(1 if detected else 0, "" if detected else self.fixture.commit, first.lock, first.content)
        self._check("moved-tag", "go", install, self.fixture.commit, int((time.monotonic() - started) * 1000))
        if detected and "checksum mismatch" not in text:
            self.results[-1]["ok"] = False
            self.results[-1]["reason"] = "refused_without_checksum_mismatch"
        _remove_tree(locked.root)
        _remove_tree(moved.root)

    def execute(self) -> None:
        self.fixture.create()
        host = stock_git.ConformanceSuite._loopback_host()
        if host is None:
            raise ConformanceError("no loopback address accepts connections")
        signed = {self.fixture.states["origin"]}
        services = {}
        try:
            for provider in ("origin", "mirror", "tampered", "retagged"):
                backend = stock_git.GitHTTPBackend(self.fixture.providers / provider, self.git, "127.0.0.1")
                services[provider] = stock_git.LocalHTTPService(backend, host)
            services["lost"] = stock_git.LocalHTTPService(stock_git.UnavailableOrigin(), host)
            relay = VerifyingRelay(signed)
            services["relay"] = stock_git.LocalHTTPService(relay, host)
            for service in services.values():
                service.start()
            base = {name: service.base_url for name, service in services.items()}

            # The relay and the source must canonicalize the same refs identically.
            probe = relay._observe(self._route("origin", base["origin"]), OWNER, REPOSITORY)
            if probe["stateHash"] != self.fixture.states["origin"]:
                raise ConformanceError("relay and source state digests disagree")

            commit = self.fixture.commit
            self._run_clients("install", base["origin"], commit, self.clients)
            self._run_clients("provider-switch", base["mirror"], commit, self.clients)

            relay.configure(self.fixture.identity, [
                self._route("origin", base["lost"]), self._route("mirror", base["mirror"])])
            self._run_clients("origin-lost", base["relay"], commit, self.clients)
            self._record_relay("origin-lost", relay)

            relay.configure(self.fixture.identity, [
                self._route("origin", base["lost"]), self._route("tampered-mirror", base["tampered"])])
            self._run_clients("unverified-fallback", base["relay"], commit, self.clients)
            self._record_relay("unverified-fallback", relay)

            missing = stable_repository.resolve(
                {"identity": self.fixture.identity, "commit": MISSING_COMMIT},
                [{"route": "origin", "reachable": True, "identity": self.fixture.identity,
                  "stateHash": self.fixture.states["origin"],
                  "hasCommit": self.fixture.has_commit("origin", MISSING_COMMIT),
                  "refs": self.fixture.refs("origin")}],
                signed)
            self.resolutions.append({"scenario": "missing-commit", "source": "reference", "requests": 1,
                                     "status": missing["status"], "selected": missing["selected"],
                                     "fallback": missing["fallback"], "errors": missing["errors"],
                                     "attempts": missing["attempts"]})
            self._run_clients("missing-commit", base["origin"], MISSING_COMMIT, self.clients)

            self._moved_tag(base["origin"], base["retagged"])

            relay.configure(self.fixture.identity, [
                self._route("origin", base["origin"]), self._route("mirror", base["mirror"])])
            self._run_clients("rollback-recovery", base["relay"], commit, self.clients)
            self._record_relay("rollback-recovery", relay)
        finally:
            for service in services.values():
                try:
                    service.close()
                except Exception:  # noqa: BLE001 - a server that never started
                    pass
        self._check_resolutions()

    def _check_resolutions(self) -> None:
        for scenario in self.contract["scenarios"]:
            wanted_status = scenario.get("resolution")
            wanted_reason = scenario.get("reason")
            rows = [row for row in self.resolutions if row["scenario"] == scenario["id"]]
            if not (wanted_status or wanted_reason) or scenario["id"] == "moved-tag" and not rows:
                continue
            reasons = {attempt["reason"] for row in rows for attempt in row["attempts"]}
            matched = bool(rows) and all(
                (not wanted_status or row["status"] == wanted_status) for row in rows
            ) and (not wanted_reason or wanted_reason in reasons)
            if not matched:
                self.results.append({
                    "scenario": scenario["id"], "client": "resolver", "expected": scenario["expect"],
                    "outcome": "resolution_mismatch", "ok": False, "reason": "resolution_mismatch",
                    "lockDigest": None, "contentDigest": None, "durationMs": 0})


def _summary(results: list[dict], resolutions: list[dict], unavailable: list[str]) -> dict:
    relay = [row for row in resolutions if row["source"] == "relay"]
    attempts = sum(row["requests"] for row in relay)
    successes = sum(row["requests"] for row in relay if row["selected"])
    failover = [row for row in relay if row["attempts"] and row["attempts"][0]["reason"] == "unreachable"]
    lock_rows = [row for row in results if row["scenario"] in REPRODUCED and row["outcome"] == "installed"]
    verification = sum(
        row["requests"] for row in resolutions for attempt in row["attempts"]
        if attempt["reason"] in {"state_unverified", "identity_mismatch", "commit_missing", "ref_moved"}
    ) + sum(1 for row in results if row["scenario"] == "moved-tag" and row["outcome"] == "refused")
    return {
        "resolutionSuccess": successes,
        "resolutionAttempts": attempts,
        "failoverSuccess": sum(row["requests"] for row in failover if row["status"] == "failed_over"),
        "failoverAttempts": sum(row["requests"] for row in failover),
        "reproducibleLocks": sum(1 for row in lock_rows if row["ok"]),
        "lockChecks": len(lock_rows),
        "verificationFailures": verification,
        "unexpectedResults": sum(1 for row in results if not row["ok"]),
        "unavailableClients": len(unavailable),
    }


def run_probe(clients: list[str] | None = None, required: list[str] | None = None,
              client_label: str = "") -> dict:
    contract = load_contract()
    problems = validate_contract(contract)
    if problems:
        raise ConformanceError("invalid contract: " + "; ".join(problems))
    executables = _client_executables()
    minimums = {item["id"]: item["minimumVersion"] for item in contract["clients"]}
    selected = list(clients or CLIENTS)
    unknown = sorted(set(selected + list(required or ())) - set(CLIENTS))
    if unknown:
        raise ConformanceError("unknown clients: " + ", ".join(unknown))
    inventory = []
    usable = []
    for client in CLIENTS:
        version = _version(executables[client], client) if client in selected else ""
        available = bool(version) and _version_tuple(version) >= _version_tuple(minimums[client])
        if not available:
            executables[client] = None if client != "git" or not version else executables[client]
        inventory.append({"id": client, "selected": client in selected,
                          "available": available, "version": version or None,
                          "minimumVersion": minimums[client]})
        if client in selected and available:
            usable.append(client)
    unavailable = [item["id"] for item in inventory if item["selected"] and not item["available"]]
    with tempfile.TemporaryDirectory(prefix="forkmesh-deps-", ignore_cleanup_errors=True) as scratch:
        suite = Suite(Path(scratch), contract, usable, executables)
        try:
            suite.execute()
        finally:
            _remove_tree(Path(scratch) / "work")
    results = suite.results
    for client in required or ():
        if client in unavailable:
            results.append({"scenario": "install", "client": client, "expected": "installed",
                            "outcome": "client_unavailable", "ok": False,
                            "reason": "required_client_unavailable", "lockDigest": None,
                            "contentDigest": None, "durationMs": 0})
    return {
        "schema": REPORT_SCHEMA,
        "suiteVersion": contract["version"],
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "clientLabel": client_label,
        "platform": {"system": platform.system(), "release": platform.release(),
                     "machine": platform.machine(), "python": platform.python_version()},
        "identity": suite.fixture.identity,
        "clients": inventory,
        "results": results,
        "resolutions": suite.resolutions,
        "relay": {"requests": sum(row["requests"] for row in suite.resolutions if row["source"] == "relay"),
                  "refused": sum(row["requests"] for row in suite.resolutions
                                 if row["source"] == "relay" and not row["selected"])},
        "summary": _summary(results, suite.resolutions, unavailable),
        "privacy": {"pathsIncluded": False, "loopbackUrlsIncluded": False,
                    "contentIncluded": False, "credentialsIncluded": False},
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="check the contract against the reference, docs, and CI")
    run = commands.add_parser("run", help="exercise stock clients against loopback providers")
    run.add_argument("--clients", default="", help="comma-separated client ids (default: all)")
    run.add_argument("--require", default="", help="comma-separated client ids that must be available")
    run.add_argument("--client-label", default="")
    run.add_argument("--output", default="", help="write the JSON report here")
    return parser


def _ids(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.command == "validate":
        problems = validate_contract(load_contract())
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1 if problems else 0
    try:
        report = run_probe(_ids(args.clients) or None, _ids(args.require), args.client_label)
    except (ConformanceError, stock_git.ConformanceError) as exc:
        print("dependency conformance could not run: %s" % exc, file=sys.stderr)
        return 2
    text = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(text, encoding="utf-8")
    print(json.dumps(report["summary"], sort_keys=True))
    for row in report["results"]:
        if not row["ok"]:
            print("unexpected: %s %s -> %s (%s)" % (row["scenario"], row["client"], row["outcome"], row["reason"]),
                  file=sys.stderr)
    return 1 if report["summary"]["unexpectedResults"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
