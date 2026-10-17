#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen

try:
    import resource
except ImportError:
    resource = None

try:
    import tomllib
except ImportError:
    tomllib = None


SCHEMA = "forkmesh-cloudflare-deploy-v1"
CONFIG_REL = ".forkmesh/cloudflare-deploy.json"
SCRIPT_REL = ".forkmesh/cloudflare-deploy.py"
DEPLOY_WORKFLOW_REL = ".forkmesh/deploy-cloudflare.yml"
ROLLBACK_WORKFLOW_REL = ".forkmesh/rollback-cloudflare.yml"
GUIDE_URL = "https://forkmesh.com/docs/cloudflare-workers/"
API_BASE = "https://api.cloudflare.com/client/v4"
WRANGLER_FILES = ("wrangler.jsonc", "wrangler.json", "wrangler.toml")
DEFAULT_RUNS_ON = "cloudflare-deploy"
DEFAULT_TOKEN_VARIABLE = "CLOUDFLARE_API_TOKEN"
DEFAULT_ACCOUNT_VARIABLE = "CLOUDFLARE_ACCOUNT_ID"
NETWORK_VARIABLE = "FORKMESH_ACTIONS_ALLOW_NETWORK"
MIN_NODE_MAJOR = 22
MIN_ADDRESS_SPACE = 32 * 1024 ** 3
LABEL_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,62}$")
VARIABLE_RE = re.compile(r"^[A-Z_][A-Z0-9_]{0,63}$")
ENVIRONMENT_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9._/-]{1,200}$")
WORKFLOW_RE = re.compile(r"^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
ACCOUNT_RE = re.compile(r"^[0-9a-f]{32}$")
ANSI_RE = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
TRIGGERS_RE = re.compile(r"^Deployed \S+ triggers")
TARGET_RE = re.compile(r"^(?:https://\S+|[A-Za-z0-9*.-]+\.[A-Za-z]{2,}(?:/\S*)?)$")
VERSION_RE = re.compile(r"Current Version ID:\s*([0-9a-fA-F-]{8,})")
LOCKFILES = (
    ("pnpm-lock.yaml", ["pnpm", "install", "--frozen-lockfile"]),
    ("yarn.lock", ["yarn", "install", "--frozen-lockfile"]),
    ("bun.lock", ["bun", "install", "--frozen-lockfile"]),
    ("bun.lockb", ["bun", "install", "--frozen-lockfile"]),
    ("package-lock.json", ["npm", "ci", "--no-audit", "--no-fund"]),
)
DEPLOY_PATTERNS = ("cloudflare/wrangler-action", "wrangler deploy", "wrangler publish")
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_CONFIG = 2
EXIT_NETWORK = 75


class DeployError(RuntimeError):
    def __init__(self, message: str, code: int = EXIT_FAILED) -> None:
        super().__init__(message)
        self.code = code


def strip_jsonc(text: str) -> str:
    out: list[str] = []
    i, n = 0, len(text)
    in_string = False
    while i < n:
        ch = text[i]
        if in_string:
            out.append(ch)
            if ch == "\\" and i + 1 < n:
                out.append(text[i + 1])
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
            out.append(ch)
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        else:
            out.append(ch)
            i += 1
    return re.sub(r",(\s*[}\]])", r"\1", "".join(out))


def parse_toml_fallback(text: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    table: list[str] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip() if not raw.strip().startswith("#") else ""
        if not line:
            continue
        header = re.fullmatch(r"\[\[?([^\]]+)\]\]?", line)
        if header:
            table = [part.strip().strip('"') for part in header.group(1).split(".")]
            node = result
            for part in table:
                node = node.setdefault(part, {})
                if not isinstance(node, dict):
                    break
            continue
        pair = re.fullmatch(r'([A-Za-z0-9_-]+)\s*=\s*"([^"]*)"', line)
        if pair:
            node = result
            for part in table:
                child = node.setdefault(part, {})
                node = child if isinstance(child, dict) else {}
            node[pair.group(1)] = pair.group(2)
    return result


def find_wrangler_config(worker_dir: Path) -> Path:
    for name in WRANGLER_FILES:
        candidate = worker_dir / name
        if candidate.is_file():
            return candidate
    raise DeployError(
        f"no {', '.join(WRANGLER_FILES)} in {worker_dir}; pass --worker-dir to the "
        "folder that holds the Worker's Wrangler configuration", EXIT_CONFIG)


def load_wrangler_config(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    try:
        if path.suffix == ".toml":
            data = tomllib.loads(text) if tomllib else parse_toml_fallback(text)
        else:
            data = json.loads(strip_jsonc(text))
    except (ValueError, TypeError) as exc:
        raise DeployError(f"{path.name} could not be read: {exc}", EXIT_CONFIG) from exc
    if not isinstance(data, dict):
        raise DeployError(f"{path.name} is not a table of settings", EXIT_CONFIG)
    return data


def wrangler_summary(path: Path) -> dict[str, Any]:
    data = load_wrangler_config(path)
    name = data.get("name")
    environments = data.get("env") if isinstance(data.get("env"), dict) else {}
    return {
        "file": path.name,
        "name": name if isinstance(name, str) else "",
        "accountId": data.get("account_id") if isinstance(data.get("account_id"), str) else "",
        "environments": sorted(k for k, v in environments.items() if isinstance(v, dict)),
        "environmentNames": {
            k: v.get("name") for k, v in environments.items()
            if isinstance(v, dict) and isinstance(v.get("name"), str)
        },
    }


def deployed_name(summary: dict[str, Any], environment: str) -> str:
    if not environment:
        return summary["name"]
    return summary["environmentNames"].get(environment) or f"{summary['name']}-{environment}"


def install_plan(repo: Path, worker_dir: Path) -> tuple[Path, list[str]]:
    repo, folder = repo.resolve(), worker_dir.resolve()
    while True:
        for name, command in LOCKFILES:
            if (folder / name).is_file():
                return folder, command
        if folder == repo or repo not in folder.parents:
            break
        folder = folder.parent
    if (worker_dir / "package.json").is_file():
        return worker_dir.resolve(), ["npm", "install", "--no-audit", "--no-fund"]
    return worker_dir.resolve(), []


def declares_wrangler(worker_dir: Path) -> bool:
    package = worker_dir / "package.json"
    if not package.is_file():
        return False
    try:
        data = json.loads(package.read_text(encoding="utf-8"))
    except ValueError:
        return False
    return any(isinstance(data.get(key), dict) and "wrangler" in data[key]
               for key in ("dependencies", "devDependencies"))


def wrangler_command(repo: Path, worker_dir: Path) -> list[str]:
    repo, folder = repo.resolve(), worker_dir.resolve()
    while True:
        binary = folder / "node_modules" / ".bin" / "wrangler"
        if binary.is_file():
            return [str(binary)]
        if folder == repo or repo not in folder.parents:
            break
        folder = folder.parent
    if declares_wrangler(worker_dir):
        return ["npx", "--no-install", "wrangler"]
    return ["npx", "--yes", "wrangler@4"]


def existing_github_deploys(repo: Path) -> list[str]:
    found = []
    workflows = repo / ".github" / "workflows"
    if not workflows.is_dir():
        return found
    for path in sorted(workflows.iterdir()):
        if path.suffix not in (".yml", ".yaml") or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if any(pattern in text for pattern in DEPLOY_PATTERNS):
            found.append(path.relative_to(repo).as_posix())
    return found


def relative_worker_dir(repo: Path, worker_dir: Path) -> str:
    try:
        rel = worker_dir.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError as exc:
        raise DeployError("--worker-dir must be inside the repository", EXIT_CONFIG) from exc
    return rel or "."


def validate_config(config: dict[str, Any]) -> dict[str, Any]:
    if config.get("schema") != SCHEMA:
        raise DeployError(f"{CONFIG_REL} is not a {SCHEMA} file", EXIT_CONFIG)
    worker_dir = config.get("workerDir", ".")
    if (not isinstance(worker_dir, str) or worker_dir.startswith("/")
            or ".." in worker_dir.split("/")):
        raise DeployError("workerDir must be a relative path inside the repository", EXIT_CONFIG)
    checks = (
        ("runsOn", LABEL_RE), ("tokenVariable", VARIABLE_RE),
        ("accountVariable", VARIABLE_RE), ("branch", BRANCH_RE),
    )
    for key, pattern in checks:
        value = config.get(key)
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise DeployError(f"{CONFIG_REL}: {key} is missing or invalid", EXIT_CONFIG)
    environment = config.get("environment", "")
    if environment and not ENVIRONMENT_RE.fullmatch(environment):
        raise DeployError(f"{CONFIG_REL}: environment is invalid", EXIT_CONFIG)
    needs = config.get("needs", [])
    if not isinstance(needs, list) or not all(
            isinstance(item, str) and WORKFLOW_RE.fullmatch(item) for item in needs):
        raise DeployError(f"{CONFIG_REL}: needs must list .forkmesh/*.yml workflows", EXIT_CONFIG)
    return config


def load_config(repo: Path) -> dict[str, Any]:
    path = repo / CONFIG_REL
    if not path.is_file():
        raise DeployError(
            f"{CONFIG_REL} is missing; run `cloudflare_worker_deploy.py connect` first",
            EXIT_CONFIG)
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as exc:
        raise DeployError(f"{CONFIG_REL} is not valid JSON: {exc}", EXIT_CONFIG) from exc
    if not isinstance(config, dict):
        raise DeployError(f"{CONFIG_REL} is not a JSON object", EXIT_CONFIG)
    return validate_config(config)


def render_deploy_workflow(config: dict[str, Any], script_rel: str = SCRIPT_REL) -> str:
    target = config.get("environment") or "default"
    lines = [
        f"# Generated by cloudflare_worker_deploy.py connect; see {GUIDE_URL}",
        f"name: Deploy Cloudflare Worker ({target})",
    ]
    if config.get("onPush"):
        lines += ["on:", "  push:", "  workflow_dispatch:"]
    else:
        lines += ["on: [workflow_dispatch]"]
    lines += [f"runs-on: {config['runsOn']}"]
    if config.get("needs"):
        lines += [f"needs: [{', '.join(config['needs'])}]"]
    lines += [
        "env:",
        f"  CLOUDFLARE_API_TOKEN: ${{{{ vars.{config['tokenVariable']} }}}}",
        f"  CLOUDFLARE_ACCOUNT_ID: ${{{{ vars.{config['accountVariable']} }}}}",
        "jobs:",
        "  deploy:",
        "    steps:",
        "      - name: Check the Worker, the token and the network",
        f"        run: python3 {script_rel} check --online",
        "      - name: Deploy with Wrangler",
        f"        run: python3 {script_rel} deploy",
        "",
    ]
    return "\n".join(lines)


def render_rollback_workflow(config: dict[str, Any], script_rel: str = SCRIPT_REL) -> str:
    target = config.get("environment") or "default"
    return "\n".join([
        f"# Generated by cloudflare_worker_deploy.py connect; see {GUIDE_URL}",
        f"name: Roll back Cloudflare Worker ({target})",
        "on: [workflow_dispatch]",
        f"runs-on: {config['runsOn']}",
        "env:",
        f"  CLOUDFLARE_API_TOKEN: ${{{{ vars.{config['tokenVariable']} }}}}",
        f"  CLOUDFLARE_ACCOUNT_ID: ${{{{ vars.{config['accountVariable']} }}}}",
        "jobs:",
        "  rollback:",
        "    steps:",
        "      - name: Check the Worker, the token and the network",
        f"        run: python3 {script_rel} check --online",
        "      - name: Roll back to the previous version",
        f"        run: python3 {script_rel} rollback",
        "",
    ])


def write_if_changed(path: Path, data: str | bytes) -> bool:
    raw = data.encode("utf-8") if isinstance(data, str) else data
    if path.is_file() and path.read_bytes() == raw:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(raw)
    os.replace(tmp, path)
    return True


def connect(*, repo: Path, worker_dir: Path | None = None, environment: str = "",
            branch: str = "main", on_push: bool = True, runs_on: str = DEFAULT_RUNS_ON,
            needs: list[str] | None = None, token_variable: str = DEFAULT_TOKEN_VARIABLE,
            account_variable: str = DEFAULT_ACCOUNT_VARIABLE, rollback: bool = True,
            keep_vars: bool = True, github: str = "", forkmesh: str = "",
            bridge_key_out: Path | None = None) -> dict[str, Any]:
    repo = Path(repo).resolve()
    if not (repo / ".git").exists():
        raise DeployError(f"{repo} is not a Git checkout", EXIT_CONFIG)
    worker_path = (repo / worker_dir).resolve() if worker_dir else repo
    summary = wrangler_summary(find_wrangler_config(worker_path))
    if not summary["name"]:
        raise DeployError(f"{summary['file']} has no top-level name", EXIT_CONFIG)
    if environment and environment not in summary["environments"]:
        known = ", ".join(summary["environments"]) or "none"
        raise DeployError(
            f"{summary['file']} has no [env.{environment}] (environments: {known})",
            EXIT_CONFIG)
    config = validate_config({
        "schema": SCHEMA,
        "workerDir": relative_worker_dir(repo, worker_path),
        "wranglerConfig": summary["file"],
        "workerName": summary["name"],
        "environment": environment,
        "branch": branch,
        "onPush": on_push,
        "runsOn": runs_on,
        "needs": list(needs or []),
        "tokenVariable": token_variable,
        "accountVariable": account_variable,
        "keepVars": keep_vars,
    })
    written = []
    if write_if_changed(repo / CONFIG_REL, json.dumps(config, indent=2, sort_keys=True) + "\n"):
        written.append(CONFIG_REL)
    source = Path(__file__).resolve()
    script = repo / SCRIPT_REL
    if script.resolve() != source and write_if_changed(script, source.read_bytes()):
        written.append(SCRIPT_REL)
    script.chmod(0o755)
    if write_if_changed(repo / DEPLOY_WORKFLOW_REL, render_deploy_workflow(config)):
        written.append(DEPLOY_WORKFLOW_REL)
    if rollback and write_if_changed(repo / ROLLBACK_WORKFLOW_REL,
                                     render_rollback_workflow(config)):
        written.append(ROLLBACK_WORKFLOW_REL)

    result: dict[str, Any] = {
        "config": CONFIG_REL,
        "workflows": [DEPLOY_WORKFLOW_REL] + ([ROLLBACK_WORKFLOW_REL] if rollback else []),
        "written": written,
        "worker": deployed_name(summary, environment),
        "runsOn": runs_on,
        "existingGitHubDeploys": existing_github_deploys(repo),
    }
    if github:
        result["github"] = connect_github(repo, github, forkmesh, bridge_key_out)
    paths = " ".join([CONFIG_REL, SCRIPT_REL] + result["workflows"])
    steps = [
        f"Desktop: Settings > Profile > Node labels, add `{runs_on}`.",
        f"Desktop: Settings > Secrets, add {token_variable} (a Cloudflare API token "
        "from the Edit Cloudflare Workers template), "
        f"{account_variable} (your 32-character account ID) and {NETWORK_VARIABLE}=1.",
        f"git add {paths} && git commit -m 'Deploy to Cloudflare from ForkMesh' "
        "and push the commit.",
        "Desktop: in the repository's Actions tab, tick Run actions on push; untick "
        "Automatically approve runs if others can push to this repository.",
        (f"Each push to {branch} deploys. " if on_push else "")
        + "Start Deploy or Roll back by hand from the desktop's Actions tab or the "
        "ForkMesh phone app.",
    ]
    if result["existingGitHubDeploys"]:
        steps.insert(0, "Turn off the GitHub deploys in "
                     + ", ".join(result["existingGitHubDeploys"])
                     + " (or Cloudflare Workers Builds) so each push deploys once.")
    result["nextSteps"] = steps + result.get("github", {}).get("nextSteps", [])
    return result


def connect_github(repo: Path, github: str, forkmesh: str,
                   key_out: Path | None) -> dict[str, Any]:
    for value, label in ((github, "--github"), (forkmesh or github, "--forkmesh")):
        if not REPOSITORY_RE.fullmatch(value):
            raise DeployError(f"{label} must be OWNER/NAME", EXIT_CONFIG)
    here = Path(__file__).resolve().parent
    candidates = [here / "github_forkmesh_bridge.py", here / "github-bridge.py"]
    bridge_path = next((path for path in candidates if path.is_file()), None)
    if bridge_path is None:
        raise DeployError(
            "github_forkmesh_bridge.py is not next to this script; run "
            "tools/github_forkmesh_bridge.py connect from a ForkMesh checkout", EXIT_CONFIG)
    bridge = sys.modules.get("github_forkmesh_bridge")
    if bridge is None:
        spec = importlib.util.spec_from_file_location("github_forkmesh_bridge", bridge_path)
        bridge = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = bridge
        spec.loader.exec_module(bridge)
    try:
        return bridge.connect(repo=repo, github_repository=github,
                              forkmesh_repository=forkmesh or github, key_out=key_out)
    except bridge.BridgeError as exc:
        raise DeployError(f"GitHub connection failed: {exc}", EXIT_CONFIG) from exc


def node_major() -> int | None:
    node = shutil.which("node")
    if not node:
        return None
    try:
        out = subprocess.run([node, "--version"], capture_output=True, text=True,
                             timeout=20, check=False).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    match = re.match(r"v(\d+)", out)
    return int(match.group(1)) if match else None


def address_space_limit() -> int | None:
    if resource is None:
        return None
    soft, _ = resource.getrlimit(resource.RLIMIT_AS)
    return None if soft == resource.RLIM_INFINITY else soft


def cloudflare_get(path: str, token: str, timeout: float = 20) -> tuple[int, dict[str, Any]]:
    request = Request(f"{API_BASE}{path}", headers={
        "Authorization": f"Bearer {token}",
        "Accept": "application/json",
        "User-Agent": "forkmesh-cloudflare-deploy/1",
    })
    try:
        with urlopen(request, timeout=timeout) as response:
            return response.status, json.loads(response.read() or b"{}")
    except HTTPError as exc:
        try:
            body = json.loads(exc.read() or b"{}")
        except ValueError:
            body = {}
        return exc.code, body if isinstance(body, dict) else {}
    except (URLError, TimeoutError, OSError) as exc:
        raise DeployError(
            f"Cloudflare's API is unreachable ({getattr(exc, 'reason', exc)}). "
            f"On the desktop, set {NETWORK_VARIABLE}=1 under Settings > Secrets so "
            "this workflow may use the network.", EXIT_NETWORK) from exc


def verify_token(token: str, account_id: str) -> str:
    status, body = cloudflare_get("/user/tokens/verify", token)
    if status != 200 and account_id:
        status, body = cloudflare_get(
            f"/accounts/{quote(account_id, safe='')}/tokens/verify", token)
    result = body.get("result") if isinstance(body.get("result"), dict) else {}
    if status != 200 or result.get("status") != "active":
        raise DeployError(
            "Cloudflare rejected the API token (HTTP "
            f"{status}); create a token with Workers Scripts: Edit and save it again")
    return str(result.get("status"))


def worker_exists(name: str, token: str, account_id: str) -> bool | None:
    status, _ = cloudflare_get(
        f"/accounts/{quote(account_id, safe='')}/workers/scripts/{quote(name, safe='')}/settings",
        token)
    if status == 200:
        return True
    if status == 404:
        return False
    return None


def credentials(config: dict[str, Any], required: bool) -> tuple[str, str]:
    token = os.environ.get("CLOUDFLARE_API_TOKEN", "").strip()
    account = os.environ.get("CLOUDFLARE_ACCOUNT_ID", "").strip()
    if required and not token:
        raise DeployError(
            f"CLOUDFLARE_API_TOKEN is empty; add {config['tokenVariable']} under the "
            "desktop's Settings > Secrets", EXIT_CONFIG)
    if required and not ACCOUNT_RE.fullmatch(account):
        raise DeployError(
            f"CLOUDFLARE_ACCOUNT_ID is not a 32-character account ID; add "
            f"{config['accountVariable']} under the desktop's Settings > Secrets",
            EXIT_CONFIG)
    return token, account


def check(repo: Path, online: bool = False) -> dict[str, Any]:
    repo = Path(repo).resolve()
    config = load_config(repo)
    worker_dir = repo / config["workerDir"]
    summary = wrangler_summary(find_wrangler_config(worker_dir))
    if summary["file"] != config.get("wranglerConfig"):
        raise DeployError(
            f"the Worker now uses {summary['file']}; run connect again", EXIT_CONFIG)
    environment = config.get("environment", "")
    if environment and environment not in summary["environments"]:
        raise DeployError(f"{summary['file']} no longer has [env.{environment}]", EXIT_CONFIG)
    report: dict[str, Any] = {
        "worker": deployed_name(summary, environment),
        "environment": environment or "default",
        "wranglerConfig": f"{config['workerDir']}/{summary['file']}".lstrip("./"),
        "install": " ".join(install_plan(repo, worker_dir)[1]) or "none",
    }
    major = node_major()
    if major is None:
        raise DeployError("Node.js is not installed on this node; Wrangler needs it",
                          EXIT_CONFIG)
    if major < MIN_NODE_MAJOR:
        raise DeployError(f"Node.js {major} is too old; Wrangler needs {MIN_NODE_MAJOR} or newer",
                          EXIT_CONFIG)
    report["node"] = major
    limit = address_space_limit()
    if limit is not None and limit < MIN_ADDRESS_SPACE:
        raise DeployError(
            f"this run may use only {limit // 1024 ** 3} GiB of virtual address space; "
            "Wrangler on Node.js 22 needs 32 GiB. Update the ForkMesh desktop.", EXIT_CONFIG)
    tool = install_plan(repo, worker_dir)[1][:1]
    if tool and not shutil.which(tool[0]):
        raise DeployError(f"{tool[0]} is not installed on this node", EXIT_CONFIG)
    if online:
        token, account = credentials(config, required=True)
        if summary["accountId"] and summary["accountId"] != account:
            raise DeployError(
                f"{summary['file']} pins account_id {summary['accountId']}, but "
                f"{config['accountVariable']} is a different account", EXIT_CONFIG)
        report["token"] = verify_token(token, account)
        exists = worker_exists(report["worker"], token, account)
        report["workerExists"] = "unknown" if exists is None else exists
    return report


class Redactor:
    def __init__(self, secrets: list[str]) -> None:
        self.secrets = [s for s in secrets if len(s) >= 6]

    def __call__(self, text: str) -> str:
        for secret in self.secrets:
            text = text.replace(secret, "***")
        return text


def run_streamed(command: list[str], cwd: Path, env: dict[str, str],
                 redact: Redactor) -> tuple[int, str]:
    print("$ " + " ".join(command), flush=True)
    try:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors="replace")
    except OSError as exc:
        raise DeployError(f"could not start {command[0]}: {exc}", EXIT_CONFIG) from exc
    captured: list[str] = []
    assert process.stdout is not None
    for line in process.stdout:
        clean = redact(line)
        captured.append(clean)
        sys.stdout.write(clean)
        sys.stdout.flush()
    return process.wait(), "".join(captured)


def parse_deploy_output(text: str) -> dict[str, Any]:
    text = ANSI_RE.sub("", text)
    urls: list[str] = []
    in_triggers = False
    for line in text.splitlines():
        if TRIGGERS_RE.match(line):
            in_triggers = True
            continue
        if in_triggers and line[:1].isspace() and line.strip():
            target = line.split()[0]
            if TARGET_RE.fullmatch(target) and target not in urls:
                urls.append(target)
            continue
        in_triggers = False
    version = VERSION_RE.search(text)
    return {"urls": urls, "versionId": version.group(1) if version else ""}


def child_env(token: str, account: str) -> dict[str, str]:
    env = dict(os.environ)
    env.update({
        "CLOUDFLARE_API_TOKEN": token,
        "CLOUDFLARE_ACCOUNT_ID": account,
        "CI": "true",
        "WRANGLER_SEND_METRICS": "false",
        "NO_UPDATE_NOTIFIER": "1",
        "NPM_CONFIG_UPDATE_NOTIFIER": "false",
    })
    return env


def deploy(repo: Path, dry_run: bool = False) -> dict[str, Any]:
    repo = Path(repo).resolve()
    config = load_config(repo)
    ref = os.environ.get("FORKMESH_REF", "")
    if ref.startswith("refs/heads/") and ref != f"refs/heads/{config['branch']}":
        return {"worker": config["workerName"], "skipped":
                f"{ref} is not refs/heads/{config['branch']}"}
    worker_dir = repo / config["workerDir"]
    token, account = credentials(config, required=not dry_run)
    env = child_env(token, account)
    redact = Redactor([token])
    install_dir, install = install_plan(repo, worker_dir)
    if install:
        code, _ = run_streamed(install, install_dir, env, redact)
        if code != 0:
            raise DeployError(f"`{' '.join(install)}` failed with exit code {code}")
    command = wrangler_command(repo, worker_dir) + ["deploy", "--config", config["wranglerConfig"]]
    if config.get("environment"):
        command += ["--env", config["environment"]]
    if config.get("keepVars", True):
        command.append("--keep-vars")
    commit = os.environ.get("FORKMESH_COMMIT", "")[:40]
    if commit and not dry_run:
        command += ["--message", f"ForkMesh {commit[:12]}"]
    if dry_run:
        command.append("--dry-run")
    code, output = run_streamed(command, worker_dir, env, redact)
    if code != 0:
        raise DeployError(f"wrangler deploy failed with exit code {code}")
    result = parse_deploy_output(output)
    result.update({"worker": config["workerName"], "environment": config.get("environment")
                   or "default", "commit": commit, "dryRun": dry_run})
    return result


def rollback(repo: Path, version: str = "") -> dict[str, Any]:
    repo = Path(repo).resolve()
    config = load_config(repo)
    worker_dir = repo / config["workerDir"]
    token, account = credentials(config, required=True)
    if version and not re.fullmatch(r"[0-9a-fA-F-]{8,64}", version):
        raise DeployError("--version must be a Worker version ID", EXIT_CONFIG)
    command = wrangler_command(repo, worker_dir) + ["rollback"]
    if version:
        command.append(version)
    command += ["--config", config["wranglerConfig"]]
    if config.get("environment"):
        command += ["--env", config["environment"]]
    command += ["--message", "Rolled back from ForkMesh"]
    code, output = run_streamed(command, worker_dir, child_env(token, account),
                                Redactor([token]))
    if code != 0:
        raise DeployError(f"wrangler rollback failed with exit code {code}")
    return {"worker": config["workerName"], **parse_deploy_output(output)}


def summary_line(kind: str, result: dict[str, Any]) -> str:
    if result.get("skipped"):
        return f"::cloudflare-{kind}:: skipped {result['skipped']}"
    parts = [f"::cloudflare-{kind}:: ok worker={result.get('worker', '')}"]
    if result.get("versionId"):
        parts.append(f"version={result['versionId']}")
    for url in result.get("urls", []):
        parts.append(f"url={url}")
    return " ".join(parts)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Deploy a Cloudflare Worker from ForkMesh Actions.")
    sub = parser.add_subparsers(dest="command", required=True)

    conn = sub.add_parser("connect", help="write the deploy config, script and workflows")
    conn.add_argument("--repo", type=Path, default=Path("."))
    conn.add_argument("--worker-dir", type=Path)
    conn.add_argument("--env", dest="environment", default="")
    conn.add_argument("--branch", default="main")
    conn.add_argument("--manual-only", action="store_true",
                      help="deploy only when started by hand, not on every push")
    conn.add_argument("--runs-on", default=DEFAULT_RUNS_ON)
    conn.add_argument("--needs", action="append", default=[])
    conn.add_argument("--token-variable", default=DEFAULT_TOKEN_VARIABLE)
    conn.add_argument("--account-variable", default=DEFAULT_ACCOUNT_VARIABLE)
    conn.add_argument("--no-rollback", action="store_true")
    conn.add_argument("--replace-vars", action="store_true",
                      help="let wrangler remove dashboard variables missing from the config")
    conn.add_argument("--github", default="", help="also connect GitHub OWNER/NAME")
    conn.add_argument("--forkmesh", default="", help="ForkMesh OWNER/NAME (default: --github)")
    conn.add_argument("--bridge-key-out", type=Path)

    chk = sub.add_parser("check", help="check the Worker config, Node.js and the token")
    chk.add_argument("--repo", type=Path, default=Path("."))
    chk.add_argument("--online", action="store_true")

    dep = sub.add_parser("deploy", help="install dependencies and run wrangler deploy")
    dep.add_argument("--repo", type=Path, default=Path("."))
    dep.add_argument("--dry-run", action="store_true")

    back = sub.add_parser("rollback", help="roll the Worker back to an earlier version")
    back.add_argument("--repo", type=Path, default=Path("."))
    back.add_argument("--version", default="")

    render = sub.add_parser("render-workflow", help="print the deploy workflow")
    render.add_argument("--repo", type=Path, default=Path("."))
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "connect":
            result = connect(
                repo=args.repo, worker_dir=args.worker_dir, environment=args.environment,
                branch=args.branch, on_push=not args.manual_only, runs_on=args.runs_on,
                needs=args.needs, token_variable=args.token_variable,
                account_variable=args.account_variable, rollback=not args.no_rollback,
                keep_vars=not args.replace_vars, github=args.github,
                forkmesh=args.forkmesh, bridge_key_out=args.bridge_key_out)
            print(json.dumps(result, indent=2))
        elif args.command == "check":
            result = check(args.repo, online=args.online)
            for key, value in result.items():
                print(f"{key}: {value}")
            print("::cloudflare-check:: ok")
        elif args.command == "deploy":
            print(summary_line("deploy", deploy(args.repo, dry_run=args.dry_run)))
        elif args.command == "rollback":
            print(summary_line("rollback", rollback(args.repo, version=args.version)))
        elif args.command == "render-workflow":
            sys.stdout.write(render_deploy_workflow(load_config(Path(args.repo).resolve())))
        return EXIT_OK
    except DeployError as exc:
        token = os.environ.get("CLOUDFLARE_API_TOKEN", "")
        message = Redactor([token])(str(exc))
        print(f"::cloudflare-{args.command}:: failed {message}", file=sys.stderr)
        return exc.code
    except OSError as exc:
        print(f"::cloudflare-{args.command}:: failed {exc}", file=sys.stderr)
        return EXIT_FAILED


if __name__ == "__main__":
    raise SystemExit(main())
