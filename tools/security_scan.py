#!/usr/bin/env python3
"""Generate a public-safe ForkMesh repository security report.

The scanner intentionally emits no source excerpts, matched credential values,
raw prompts, or exploit details.  Public finding identifiers are derived only
from the non-secret rule and location; matched evidence never contributes to a
public digest.  Dependency names and versions are sent to OSV only when they
came from public-registry lockfile entries.
"""

from __future__ import annotations

import argparse
import ast
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import tomllib
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_POLICY = ROOT / "www" / "docs" / "security-scan-policy.json"
DEFAULT_SCHEMA = ROOT / "www" / "docs" / "security-scan.schema.json"
DEFAULT_CLIPBOARD_SCHEMA = ROOT / "www" / "docs" / "security-clipboard.schema.json"
SCANNER_NAME = "ForkMesh public-safe scanner"
SCANNER_VERSION = "1.0.0"
OSV_BATCH_URL = "https://api.osv.dev/v1/querybatch"
INGEST_RETRYABLE_HTTP = frozenset({408, 409, 425, 429, 500, 502, 503, 504})
INGEST_PATH_RE = re.compile(
    r"^/api/repo/([A-Za-z0-9._:-]{1,80})/"
    r"([A-Za-z0-9._:-]{1,80})/security-scans/ingest$")
SEVERITIES = ("critical", "high", "medium", "low", "info")
CATEGORIES = ("dependency", "secret", "static")
NOTICES = [
    "Automated scans may miss vulnerabilities.",
    "Results apply only to the scanned commit.",
    "A clean scan is not a guarantee of security; human review may still be required.",
]
MAX_PUBLIC_FINDINGS = 500
MAX_PUBLIC_RECOMMENDATIONS = 100


class ScanError(RuntimeError):
    """A public-safe scanner failure."""


@dataclass(frozen=True, order=True)
class Dependency:
    ecosystem: str
    name: str
    version: str
    manifest: str


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _relative(path: Path, root: Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.name


def _git_commit(root: Path) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            check=True,
            text=True,
            capture_output=True,
        )
        commit = result.stdout.strip().lower()
        return commit if re.fullmatch(r"[0-9a-f]{40}", commit) else "unknown"
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def _tracked_files(root: Path) -> list[Path]:
    try:
        result = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z"],
            check=True,
            capture_output=True,
        )
        return [
            root / raw.decode("utf-8", "surrogateescape")
            for raw in result.stdout.split(b"\0")
            if raw
        ]
    except (OSError, subprocess.CalledProcessError):
        return [path for path in root.rglob("*") if path.is_file()]


def _safe_repository_file(root: Path, path: Path) -> bool:
    """Return true only for a regular file contained by the scan root.

    Git repositories may contain symbolic links.  Following one while the
    authenticated publisher token is present could expose host files to the
    scanner, so both source scans and dependency inventories fail closed for
    links and paths that resolve outside the checked-out repository.
    """

    try:
        if path.is_symlink():
            return False
        resolved_root = root.resolve(strict=True)
        resolved_path = path.resolve(strict=True)
        resolved_path.relative_to(resolved_root)
        return resolved_path.is_file()
    except (OSError, RuntimeError, ValueError):
        return False


def _excluded(relative: str, globs: Iterable[str]) -> bool:
    return any(fnmatch.fnmatch(relative, pattern) for pattern in globs)


def select_scan_files(
    root: Path, policy: dict[str, Any]
) -> tuple[list[Path], int]:
    extensions = {str(item).lower() for item in policy["includeExtensions"]}
    excluded_globs = [str(item) for item in policy["excludeGlobs"]]
    maximum = int(policy["maximumFileBytes"])
    selected: list[Path] = []
    excluded_count = 0
    for path in _tracked_files(root):
        if not _safe_repository_file(root, path):
            excluded_count += 1
            continue
        relative = _relative(path, root)
        try:
            size = path.stat().st_size
        except OSError:
            excluded_count += 1
            continue
        if (
            not path.is_file()
            or path.suffix.lower() not in extensions
            or size > maximum
            or _excluded(relative, excluded_globs)
        ):
            excluded_count += 1
            continue
        selected.append(path)
    return sorted(selected), excluded_count


def _fingerprint(rule_id: str, path: str, line: int | None) -> str:
    """Return a stable public ID without creating a secret-value oracle.

    A digest of matched evidence still lets an observer test guesses for a
    short or structured credential.  Rule, normalized public path, and line
    are sufficient to correlate the same public finding across scans, so those
    are the only inputs permitted here.
    """
    material = f"forkmesh-public-location-v2\0{rule_id}\0{path}\0{line}"
    return hashlib.sha256(material.encode("utf-8", "replace")).hexdigest()[:16]


def make_finding(
    *,
    category: str,
    severity: str,
    rule_id: str,
    path: str,
    line: int | None,
    summary: str,
    recommendation: str,
    advisory_ids: list[str] | None = None,
) -> dict[str, Any]:
    clean_path = "".join(
        character
        for character in str(path or "").replace("\\", "/")
        if character >= " " and character != "\x7f"
    )
    if clean_path.startswith("/") or ".." in clean_path.split("/"):
        clean_path = "redacted-path-" + hashlib.sha256(
            clean_path.encode("utf-8", "replace")
        ).hexdigest()[:16]
    clean_path = clean_path[:500]
    clean_rule_id = str(rule_id or "")[:120]
    fingerprint = _fingerprint(clean_rule_id, clean_path, line)
    finding: dict[str, Any] = {
        "id": fingerprint,
        "category": category,
        "severity": severity if severity in SEVERITIES else "info",
        "ruleId": clean_rule_id,
        "path": clean_path,
        "line": line,
        "summary": summary[:300],
        "recommendation": recommendation[:500],
        "evidence": {"redacted": True, "fingerprint": fingerprint},
        "falsePositiveStatus": "unreviewed",
    }
    if advisory_ids:
        finding["advisoryIds"] = [
            str(value)[:100]
            for value in sorted(set(advisory_ids))[:20]
        ]
    return finding


def _secret_gate() -> Any:
    """The shared secret-scan contract (tools/secret_gate.py), loaded by path."""
    module = sys.modules.get("secret_gate")
    if module is not None:
        return module
    import importlib.util

    spec = importlib.util.spec_from_file_location(
        "secret_gate", Path(__file__).resolve().parent / "secret_gate.py")
    if spec is None or spec.loader is None:
        raise ScanError("secret_gate.py is unavailable.")
    module = importlib.util.module_from_spec(spec)
    sys.modules["secret_gate"] = module
    spec.loader.exec_module(module)
    return module


def scan_secrets(
    root: Path, files: Iterable[Path], policy: dict[str, Any]
) -> list[dict[str, Any]]:
    gate = _secret_gate()
    if policy.get("secretContract") != gate.CONTRACT_ID:
        raise ScanError("Scan policy names an unsupported secret contract.")
    findings: list[dict[str, Any]] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        relative = _relative(path, root)
        for rule, line in gate.scan_text(text):
            # The match proves the rule fired, but the matched value is
            # neither copied nor hashed into any public artifact.
            findings.append(
                make_finding(
                    category="secret",
                    severity=rule["severity"],
                    rule_id=rule["id"],
                    path=relative,
                    line=line,
                    summary=rule["summary"],
                    recommendation=rule["recommendation"],
                )
            )
    return findings


class PythonStaticVisitor(ast.NodeVisitor):
    def __init__(self, relative_path: str) -> None:
        self.path = relative_path
        self.findings: list[dict[str, Any]] = []

    @staticmethod
    def _call_name(node: ast.AST) -> str:
        if isinstance(node, ast.Name):
            return node.id
        if isinstance(node, ast.Attribute):
            prefix = PythonStaticVisitor._call_name(node.value)
            return f"{prefix}.{node.attr}" if prefix else node.attr
        return ""

    def _add(
        self,
        node: ast.AST,
        rule_id: str,
        severity: str,
        summary: str,
        recommendation: str,
    ) -> None:
        line = int(getattr(node, "lineno", 1))
        self.findings.append(
            make_finding(
                category="static",
                severity=severity,
                rule_id=rule_id,
                path=self.path,
                line=line,
                summary=summary,
                recommendation=recommendation,
            )
        )

    def visit_Call(self, node: ast.Call) -> None:
        name = self._call_name(node.func)
        if name in {"eval", "exec", "builtins.eval", "builtins.exec"}:
            self._add(
                node,
                "python-dynamic-execution",
                "high",
                "Dynamic Python execution requires security review.",
                "Replace dynamic execution with explicit parsing or a constrained dispatcher.",
            )
        if name in {"subprocess.run", "subprocess.call", "subprocess.Popen"}:
            shell_true = any(
                keyword.arg == "shell"
                and isinstance(keyword.value, ast.Constant)
                and keyword.value.value is True
                for keyword in node.keywords
            )
            if shell_true:
                self._add(
                    node,
                    "python-subprocess-shell",
                    "high",
                    "A subprocess call enables shell parsing.",
                    "Pass an argument list without shell=True and validate all external input.",
                )
        if name in {"pickle.load", "pickle.loads", "dill.load", "dill.loads"}:
            self._add(
                node,
                "python-unsafe-deserialization",
                "high",
                "Potentially unsafe object deserialization requires review.",
                "Use a data-only format and validate its schema before processing.",
            )
        if name in {"hashlib.md5", "hashlib.sha1"}:
            self._add(
                node,
                "python-weak-digest",
                "low",
                "A legacy digest algorithm is used.",
                "Use SHA-256 or stronger unless this is a non-security compatibility checksum.",
            )
        self.generic_visit(node)


def scan_static(
    root: Path, files: Iterable[Path], policy: dict[str, Any]
) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    js_rules = [
        (
            re.compile(r"\beval\s*\("),
            "javascript-dynamic-execution",
            "high",
            "Dynamic JavaScript execution requires security review.",
            "Use structured parsing and explicit dispatch instead of eval.",
        ),
        (
            re.compile(r"\b(?:child_process|childProcess)\.exec\s*\("),
            "javascript-shell-exec",
            "high",
            "A JavaScript process API may invoke a shell command.",
            "Use an argument-array process API and validate external input.",
        ),
    ]
    excluded_globs = [str(item) for item in policy.get("staticExcludeGlobs", [])]
    for path in files:
        relative = _relative(path, root)
        if _excluded(relative, excluded_globs):
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if path.suffix.lower() == ".py":
            try:
                tree = ast.parse(text, filename=relative)
            except SyntaxError:
                findings.append(
                    make_finding(
                        category="static",
                        severity="medium",
                        rule_id="python-parse-failed",
                        path=relative,
                        line=None,
                        summary="Python static analysis could not parse this file.",
                        recommendation="Confirm the file syntax and scanner compatibility.",
                    )
                )
                continue
            visitor = PythonStaticVisitor(relative)
            visitor.visit(tree)
            findings.extend(visitor.findings)
        elif path.suffix.lower() in {".js", ".ts", ".tsx"}:
            for pattern, rule_id, severity, summary, recommendation in js_rules:
                for match in pattern.finditer(text):
                    line = text.count("\n", 0, match.start()) + 1
                    findings.append(
                        make_finding(
                            category="static",
                            severity=severity,
                            rule_id=rule_id,
                            path=relative,
                            line=line,
                            summary=summary,
                            recommendation=recommendation,
                        )
                    )
    unique = {finding["id"]: finding for finding in findings}
    return list(unique.values())


def _package_lock_dependencies(path: Path, root: Path) -> set[Dependency]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set()
    result: set[Dependency] = set()
    for package_path, metadata in (payload.get("packages") or {}).items():
        if not package_path or not isinstance(metadata, dict):
            continue
        marker = "node_modules/"
        if marker not in package_path:
            continue
        name = package_path.rsplit(marker, 1)[-1]
        version = str(metadata.get("version") or "")
        resolved = str(metadata.get("resolved") or "")
        if (
            name
            and version
            and (not resolved or "registry.npmjs.org" in resolved)
            and not version.startswith(("file:", "git+", "http:", "https:"))
        ):
            result.add(Dependency("npm", name, version, _relative(path, root)))
    return result


def _pylock_dependencies(path: Path, root: Path) -> set[Dependency]:
    try:
        payload = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError):
        return set()
    result: set[Dependency] = set()
    for package in payload.get("packages", []):
        if not isinstance(package, dict):
            continue
        name = str(package.get("name") or "")
        version = str(package.get("version") or "")
        wheels = package.get("wheels") or []
        public = any(
            "files.pythonhosted.org" in str(wheel.get("url") or "")
            for wheel in wheels
            if isinstance(wheel, dict)
        )
        if name and version and public:
            result.add(Dependency("PyPI", name, version, _relative(path, root)))
    return result


def _pub_lock_dependencies(path: Path, root: Path) -> set[Dependency]:
    # pubspec.lock is simple YAML, but pulling in a YAML parser solely for the
    # inventory would expand the scanner's supply chain.  The generated lockfile
    # format has stable two-space package keys and four-space version fields.
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        return set()
    result: set[Dependency] = set()
    current = ""
    public_hosted = False
    for line in lines:
        package = re.match(r"^  ([A-Za-z0-9_.-]+):\s*$", line)
        if package:
            current = package.group(1)
            public_hosted = False
            continue
        if not current:
            continue
        if re.match(r"^\s{4}source:\s+hosted\s*$", line):
            public_hosted = True
            continue
        version = re.match(r'^\s{4}version:\s+"?([^"\s]+)"?\s*$', line)
        if version and public_hosted and version.group(1) != "0.0.0":
            result.add(
                Dependency("Pub", current, version.group(1), _relative(path, root))
            )
    return result


def dependency_inventory(root: Path) -> list[Dependency]:
    result: set[Dependency] = set()
    for path in _tracked_files(root):
        if not _safe_repository_file(root, path):
            continue
        if path.name in {"package-lock.json", "npm-shrinkwrap.json"}:
            result.update(_package_lock_dependencies(path, root))
        elif path.name == "pylock.toml":
            result.update(_pylock_dependencies(path, root))
        elif path.name == "pubspec.lock":
            result.update(_pub_lock_dependencies(path, root))
    return sorted(result)


def _osv_severity(vulnerability: dict[str, Any]) -> str:
    database = vulnerability.get("database_specific")
    if isinstance(database, dict):
        value = str(database.get("severity") or "").lower()
        if value in SEVERITIES:
            return value
        aliases = {"moderate": "medium", "important": "high"}
        if value in aliases:
            return aliases[value]
    for item in vulnerability.get("severity") or []:
        if not isinstance(item, dict):
            continue
        score = str(item.get("score") or "")
        match = re.search(r"(?:^|/)(\d+(?:\.\d+)?)$", score)
        if not match:
            continue
        numeric = float(match.group(1))
        if numeric >= 9:
            return "critical"
        if numeric >= 7:
            return "high"
        if numeric >= 4:
            return "medium"
        if numeric > 0:
            return "low"
    return "medium"


def query_osv(
    dependencies: list[Dependency],
    *,
    opener: Callable[..., Any] = urlopen,
    sleeper: Callable[[float], None] = time.sleep,
    attempts: int = 4,
) -> list[dict[str, Any]]:
    if not dependencies:
        return []
    queries = [
        {
            "package": {"ecosystem": dependency.ecosystem, "name": dependency.name},
            "version": dependency.version,
        }
        for dependency in dependencies
    ]
    request = Request(
        OSV_BATCH_URL,
        data=json.dumps({"queries": queries}).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "forkmesh-security-scan/1",
        },
    )
    payload: dict[str, Any] | None = None
    for attempt in range(attempts):
        try:
            with opener(request, timeout=45) as response:
                payload = json.loads(response.read().decode("utf-8"))
                break
        except HTTPError as exc:
            if exc.code not in {408, 425, 429, 500, 502, 503, 504} or attempt + 1 >= attempts:
                raise ScanError(f"OSV returned HTTP {exc.code}") from exc
        except (URLError, TimeoutError) as exc:
            if attempt + 1 >= attempts:
                reason = getattr(exc, "reason", str(exc))
                raise ScanError(f"OSV connection failed: {reason}") from exc
        sleeper(min(8.0, 0.75 * (2**attempt)))
    results = payload.get("results", []) if isinstance(payload, dict) else []
    findings: list[dict[str, Any]] = []
    for dependency, result in zip(dependencies, results):
        vulnerabilities = result.get("vulns", []) if isinstance(result, dict) else []
        for vulnerability in vulnerabilities:
            if not isinstance(vulnerability, dict):
                continue
            advisory_ids = [
                str(value)
                for value in [vulnerability.get("id"), *(vulnerability.get("aliases") or [])]
                if value
            ]
            rule_id = str(vulnerability.get("id") or "osv-advisory")
            findings.append(
                make_finding(
                    category="dependency",
                    severity=_osv_severity(vulnerability),
                    rule_id=rule_id,
                    path=dependency.manifest,
                    line=None,
                    summary=(
                        f"A known advisory applies to {dependency.name} "
                        f"{dependency.version}."
                    ),
                    recommendation=(
                        "Review the listed advisory IDs, upgrade to a fixed "
                        "version, and retest affected behavior."
                    ),
                    advisory_ids=advisory_ids,
                )
            )
    return findings


def validate_public_report(report: dict[str, Any]) -> None:
    required = {
        "schemaVersion",
        "visibility",
        "generatedAt",
        "repository",
        "scanner",
        "policy",
        "scan",
        "status",
        "summary",
        "findings",
        "recommendations",
        "notices",
    }
    missing = required.difference(report)
    if missing:
        raise ScanError(f"generated report is missing fields: {sorted(missing)}")
    visibility = report["visibility"]
    if visibility != {
        "public": True,
        "redacted": True,
        "containsSourceExcerpts": False,
        "containsSecretValues": False,
    }:
        raise ScanError("generated report is not marked public-safe and redacted")
    for finding in report["findings"]:
        if finding.get("evidence", {}).get("redacted") is not True:
            raise ScanError("finding contains unredacted evidence")
        if set(finding.get("evidence", {})) != {"redacted", "fingerprint"}:
            raise ScanError("finding evidence contains unsupported public fields")
    if report["notices"] != NOTICES:
        raise ScanError("required automated-scan limitations are missing")


def clipboard_report(report: dict[str, Any]) -> dict[str, Any]:
    """Convert the rich artifact into the compact world clipboard contract."""

    validate_public_report(report)
    severity = report["summary"]["bySeverity"]
    categories = report["summary"]["byCategory"]
    model = report["scanner"]["model"]
    review_counts = {"unreviewed": 0, "confirmed": 0, "dismissed": 0}
    for finding in report["findings"]:
        review_counts[finding["falsePositiveStatus"]] += 1
    compact = {
        "schemaVersion": 1,
        "status": report["status"],
        "scannedAt": report["scan"]["completedAt"],
        "scanner": (
            f"{report['scanner']['name']} {report['scanner']['version']}".strip()
        ),
        "model": str(model.get("name") or ""),
        "modelVersion": str(model.get("version") or ""),
        "policy": (
            f"{report['policy']['id']}@{report['policy']['version']}"
            f" sha256:{report['policy']['sha256']}"
        ),
        "durationMs": int(report["scan"]["durationMs"]),
        "commitHash": report["repository"]["commit"],
        "included": list(report["scan"]["areasIncluded"]),
        "excluded": list(report["scan"]["areasExcluded"]),
        "findings": {
            "critical": int(severity["critical"]),
            "high": int(severity["high"]),
            "medium": int(severity["medium"]),
            "low": int(severity["low"]),
            "informational": int(severity["info"]),
        },
        "categories": {
            "dependency": int(categories["dependency"]),
            "secretDetection": int(categories["secret"]),
            "staticAnalysis": int(categories["static"]),
        },
        "recommendations": list(report["recommendations"]),
        "reviewStatus": "Not reviewed",
        "falsePositiveStatus": review_counts,
        "publicRedaction": {
            "sourceCode": True,
            "secrets": True,
            "sensitivePrompts": True,
            "exploitDetails": True,
        },
        "scopeLimitations": list(report["scan"]["scopeLimitations"]),
        "limitations": list(report["notices"]),
    }
    return compact


def publish_scan_ingest(
    rich_report: dict[str, Any],
    compact_report: dict[str, Any],
    *,
    url: str,
    token: str,
    opener: Callable[..., Any] = urlopen,
    sleeper: Callable[[float], None] = time.sleep,
    attempts: int = 4,
) -> None:
    """Publish both redacted schemas without logging credentials or payloads."""

    parsed = urlparse(url)
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ScanError(
            "security scan ingest URL must be HTTPS and contain no credentials, "
            "query, or fragment"
        )
    route = INGEST_PATH_RE.fullmatch(parsed.path)
    if not route:
        raise ScanError(
            "security scan ingest URL must use "
            "/api/repo/<owner>/<repo>/security-scans/ingest"
        )
    if (
        len(token) < 32
        or len(token) > 4096
        or any(character.isspace() or ord(character) < 33 for character in token)
    ):
        raise ScanError(
            "security scan ingest token must contain at least 32 "
            "non-whitespace characters"
        )
    validate_public_report(rich_report)
    if compact_report != clipboard_report(rich_report):
        raise ScanError(
            "security scan clipboard does not match the rich report")
    route_repository = route.group(1) + "/" + route.group(2)
    if route_repository.lower() != rich_report["repository"]["name"].lower():
        raise ScanError(
            "security scan ingest URL repository does not match the report")
    payload = {
        "schemaVersion": 1,
        "type": "forkmesh.security-scan-ingest",
        "rich": rich_report,
        "clipboard": compact_report,
    }
    encoded = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    for attempt in range(attempts):
        request = Request(
            url,
            data=encoded,
            method="POST",
            headers={
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json",
                "Accept": "application/json",
                "User-Agent": "forkmesh-security-scan-publisher/1",
                "X-ForkMesh-Artifact-Schema": "security-scan-v1",
            },
        )
        try:
            with opener(request, timeout=30) as response:
                if 200 <= int(response.status) < 300:
                    return
                raise ScanError(
                    f"security scan ingest returned HTTP {int(response.status)}"
                )
        except HTTPError as exc:
            if exc.code in INGEST_RETRYABLE_HTTP and attempt + 1 < attempts:
                retry_after = exc.headers.get("Retry-After") if exc.headers else None
                try:
                    delay = min(15.0, max(0.0, float(retry_after)))
                except (TypeError, ValueError):
                    delay = min(8.0, 0.75 * (2**attempt))
                sleeper(delay)
                continue
            # Do not read or print an endpoint response body. It may contain
            # deployment diagnostics that do not belong in public logs.
            raise ScanError(
                f"security scan ingest returned HTTP {exc.code}"
            ) from exc
        except (URLError, TimeoutError) as exc:
            if attempt + 1 < attempts:
                sleeper(min(8.0, 0.75 * (2**attempt)))
                continue
            raise ScanError("security scan ingest connection failed") from exc
    raise ScanError("security scan ingest retry budget exhausted")


def build_report(
    root: Path,
    *,
    repository_name: str,
    policy_path: Path = DEFAULT_POLICY,
    offline: bool = False,
    osv_opener: Callable[..., Any] = urlopen,
) -> dict[str, Any]:
    started_clock = time.monotonic()
    started_at = utc_now()
    policy_bytes = policy_path.read_bytes()
    policy = json.loads(policy_bytes.decode("utf-8"))
    files, files_excluded = select_scan_files(root, policy)
    inventory = dependency_inventory(root)
    scope_limitations: list[str] = []

    findings = scan_secrets(root, files, policy)
    findings.extend(scan_static(root, files, policy))
    advisory_source = "offline" if offline else "OSV"
    if offline:
        scope_limitations.append(
            "Dependency inventory was collected, but online vulnerability advisories were not queried."
        )
    else:
        try:
            findings.extend(query_osv(inventory, opener=osv_opener))
        except ScanError as exc:
            advisory_source = "offline"
            scope_limitations.append(
                "The OSV advisory service was unavailable; dependency results are incomplete."
            )
            # The exception detail is intentionally not included in the public
            # artifact because upstream responses can contain request metadata.
            print(f"security scan note: {exc}", file=sys.stderr)

    findings.sort(
        key=lambda item: (
            SEVERITIES.index(item["severity"]),
            item["category"],
            item["path"],
            item["line"] or 0,
            item["ruleId"],
        )
    )
    if len(findings) > MAX_PUBLIC_FINDINGS:
        omitted = len(findings) - MAX_PUBLIC_FINDINGS
        findings = findings[:MAX_PUBLIC_FINDINGS]
        scope_limitations.append(
            "The public report retained the 500 highest-priority findings and "
            "omitted %d additional findings; review the private workflow logs "
            "and split the scan scope before relying on this result." % omitted
        )
    severity_counts = Counter(item["severity"] for item in findings)
    category_counts = Counter(item["category"] for item in findings)
    if severity_counts["critical"]:
        status = "Critical findings detected"
    elif findings:
        status = "Review required"
    elif scope_limitations:
        status = "Scope limited"
    else:
        status = "No known critical findings detected"
    completion = "scope_limited" if scope_limitations else "completed"

    all_recommendations = sorted(
        {str(item["recommendation"]) for item in findings})
    recommendations = all_recommendations[:MAX_PUBLIC_RECOMMENDATIONS]
    if len(all_recommendations) > MAX_PUBLIC_RECOMMENDATIONS:
        scope_limitations.append(
            "The public report retained the first 100 distinct recommendations; "
            "review individual findings for the complete action set."
        )
    completed_at = utc_now()
    duration_ms = max(0, int((time.monotonic() - started_clock) * 1000))
    report = {
        "schemaVersion": 1,
        "visibility": {
            "public": True,
            "redacted": True,
            "containsSourceExcerpts": False,
            "containsSecretValues": False,
        },
        "generatedAt": completed_at,
        "repository": {
            "name": repository_name,
            "commit": _git_commit(root),
        },
        "scanner": {
            "name": SCANNER_NAME,
            "version": SCANNER_VERSION,
            "model": {"used": False, "name": None, "version": None},
        },
        "policy": {
            "id": str(policy["id"]),
            "version": str(policy["version"]),
            "path": _relative(policy_path, root),
            "sha256": hashlib.sha256(policy_bytes).hexdigest(),
        },
        "scan": {
            "startedAt": started_at,
            "completedAt": completed_at,
            "durationMs": duration_ms,
            "completion": completion,
            "areasIncluded": [
                "public-registry dependency advisories",
                "credential-pattern detection",
                "Python and JavaScript static checks",
            ],
            "areasExcluded": [
                *[str(item) for item in policy["excludeGlobs"]],
                *[
                    f"static-analysis-only:{item}"
                    for item in policy.get("staticExcludeGlobs", [])
                ],
            ],
            "filesInspected": len(files),
            "filesExcluded": files_excluded,
            "dependencyInventoryCount": len(inventory),
            "dependencyAdvisorySource": advisory_source,
            "scopeLimitations": scope_limitations,
        },
        "status": status,
        "summary": {
            "total": len(findings),
            "bySeverity": {
                severity: severity_counts[severity] for severity in SEVERITIES
            },
            "byCategory": {
                category: category_counts[category] for category in CATEGORIES
            },
        },
        "findings": findings,
        "recommendations": recommendations,
        "notices": list(NOTICES),
    }
    validate_public_report(report)
    return report


def markdown_summary(report: dict[str, Any]) -> str:
    summary = report["summary"]
    return (
        "## ForkMesh daily security scan\n\n"
        f"- Status: **{report['status']}**\n"
        f"- Commit: `{report['repository']['commit']}`\n"
        f"- Duration: {report['scan']['durationMs']} ms\n"
        f"- Files inspected: {report['scan']['filesInspected']}\n"
        f"- Dependency inventory: {report['scan']['dependencyInventoryCount']}\n"
        f"- Findings: {summary['total']} "
        f"(critical {summary['bySeverity']['critical']}, "
        f"high {summary['bySeverity']['high']}, "
        f"medium {summary['bySeverity']['medium']}, "
        f"low {summary['bySeverity']['low']})\n"
        f"- Public artifact redacted: yes\n\n"
        + "\n".join(f"> {notice}" for notice in report["notices"])
        + "\n"
    )


def failed_report(
    root: Path,
    *,
    repository_name: str,
    policy_path: Path,
) -> dict[str, Any]:
    """Return a schema-compatible public artifact for a fatal scanner error."""

    try:
        policy_bytes = policy_path.read_bytes()
        policy = json.loads(policy_bytes.decode("utf-8"))
        policy_id = str(policy.get("id") or "unknown")
        policy_version = str(policy.get("version") or "unknown")
        policy_digest = hashlib.sha256(policy_bytes).hexdigest()
    except (OSError, ValueError, json.JSONDecodeError):
        policy_id = "unknown"
        policy_version = "unknown"
        policy_digest = "0" * 64
    now = utc_now()
    report = {
        "schemaVersion": 1,
        "visibility": {
            "public": True,
            "redacted": True,
            "containsSourceExcerpts": False,
            "containsSecretValues": False,
        },
        "generatedAt": now,
        "repository": {"name": repository_name, "commit": _git_commit(root)},
        "scanner": {
            "name": SCANNER_NAME,
            "version": SCANNER_VERSION,
            "model": {"used": False, "name": None, "version": None},
        },
        "policy": {
            "id": policy_id,
            "version": policy_version,
            "path": _relative(policy_path, root),
            "sha256": policy_digest,
        },
        "scan": {
            "startedAt": now,
            "completedAt": now,
            "durationMs": 0,
            "completion": "failed",
            "areasIncluded": [],
            "areasExcluded": [],
            "filesInspected": 0,
            "filesExcluded": 0,
            "dependencyInventoryCount": 0,
            "dependencyAdvisorySource": "offline",
            "scopeLimitations": [
                "The scanner did not complete. Public output omits private diagnostic details."
            ],
        },
        "status": "Scan failed",
        "summary": {
            "total": 0,
            "bySeverity": {severity: 0 for severity in SEVERITIES},
            "byCategory": {category: 0 for category in CATEGORIES},
        },
        "findings": [],
        "recommendations": [
            "Review the private workflow diagnostics, correct the scanner failure, and rerun it."
        ],
        "notices": list(NOTICES),
    }
    validate_public_report(report)
    return report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument(
        "--repository",
        default=os.environ.get("GITHUB_REPOSITORY", "local/forkmesh"),
    )
    parser.add_argument("--policy", type=Path, default=DEFAULT_POLICY)
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    parser.add_argument(
        "--clipboard-schema", type=Path, default=DEFAULT_CLIPBOARD_SCHEMA
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--clipboard-output",
        type=Path,
        help="defaults to security-clipboard.json next to --output",
    )
    parser.add_argument("--summary", type=Path)
    parser.add_argument(
        "--offline",
        action="store_true",
        help="collect dependency inventory without sending public package coordinates to OSV",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    clipboard_output = args.clipboard_output or args.output.with_name(
        "security-clipboard.json"
    )

    def write_artifacts(report: dict[str, Any]) -> dict[str, Any]:
        compact = clipboard_report(report)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        clipboard_output.parent.mkdir(parents=True, exist_ok=True)
        clipboard_output.write_text(
            json.dumps(compact, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if args.summary:
            args.summary.parent.mkdir(parents=True, exist_ok=True)
            args.summary.write_text(markdown_summary(report), encoding="utf-8")
        return compact

    try:
        # Loading the schema catches accidental invalid JSON in the published
        # contract even when the optional jsonschema package is unavailable.
        json.loads(args.schema.read_text(encoding="utf-8"))
        json.loads(args.clipboard_schema.read_text(encoding="utf-8"))
        report = build_report(
            args.root.resolve(),
            repository_name=args.repository,
            policy_path=args.policy.resolve(),
            offline=args.offline,
        )
        compact = write_artifacts(report)
        summary = markdown_summary(report)
        print(summary)
    except (ScanError, OSError, ValueError, json.JSONDecodeError) as exc:
        try:
            report = failed_report(
                args.root.resolve(),
                repository_name=args.repository,
                policy_path=args.policy.resolve(),
            )
            write_artifacts(report)
        except OSError:
            pass
        print(f"security scan failed: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
