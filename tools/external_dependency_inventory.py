#!/usr/bin/env python3
"""Inventory outside services and check them against the dependency map (issue #503).

The map lives in ``conformance/external-dependencies/v1/manifest.json``; the
human page ``www/docs/external-dependency-map.md`` carries a generated block
rendered from it. This tool scans production source for every outside host a
ForkMesh surface can contact, every Worker secret that authenticates to a
provider, and every D1 table that stores a provider's identifier, then fails
when any of them is not classified by the manifest.

    python3 tools/external_dependency_inventory.py check     # exit 1 on drift
    python3 tools/external_dependency_inventory.py report    # JSON inventory
    python3 tools/external_dependency_inventory.py render    # rewrite the doc block

Standard library only, so it runs on every CI platform without installs.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "conformance" / "external-dependencies" / "v1" / "manifest.json"
SCHEMA_NAME = "forkmesh-external-dependency-map-v1"
DOC_BEGIN = "<!-- external-dependency-map:begin -->"
DOC_END = "<!-- external-dependency-map:end -->"

NECESSITIES = ("mandatory", "degradable", "optional", "operator", "build")
CATEGORIES = (
    "hosting", "dns", "certificates", "cdn-edge", "forge-api", "identity",
    "email", "payments", "blockchain", "package-registry", "telemetry",
    "hosted-runner", "ai-provider", "media-embed", "social", "infrastructure",
)
REFERENCE_KINDS = ("link", "namespace", "documentation")

_HOST_RE = re.compile(
    r"\b(?:https?|wss?)://(?:[^\s/@\"'`<>]*@)?"
    r"(?P<host>[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?)+)"
)
# Text immediately before a URL that makes a browser or client fetch it
# without a user following a link. Quotes may be backslash-escaped because
# markup is often built inside Python, JavaScript, and C++ string literals.
_Q = r"\\?[\"'`]"
_SUBRESOURCE_PREFIX = re.compile(
    rf"(?:\b(?:src|srcset|poster|data|action)\s*=\s*(?:{_Q})?"
    rf"|\bfetch\s*\(\s*{_Q}"
    rf"|\burl\(\s*(?:{_Q})?"
    rf"|\bimport\s*\(?\s*{_Q}"
    rf"|\bnew\s+(?:WebSocket|EventSource|Worker)\s*\(\s*{_Q}"
    rf"|\burlopen\s*\(\s*{_Q}"
    rf"|\bUri\.parse\s*\(\s*{_Q}"
    r"|\bhttp\.(?:Get|Post|NewRequest)\s*\([^\"]*\"?)$"
)
_LINK_TAG_FETCHES = re.compile(
    r"<link\b[^>]*\brel\s*=\s*[\"']?(?:stylesheet|preload|preconnect|"
    r"modulepreload|dns-prefetch|icon)", re.IGNORECASE)
_RESERVED_SUFFIXES = (
    ".example", ".test", ".invalid", ".localhost", ".local", ".internal",
    ".lan", ".home.arpa",
)
_RESERVED_DOMAINS = ("example.com", "example.org", "example.net", "localhost")
_IP_RE = re.compile(r"^\d{1,3}(?:\.\d{1,3}){3}$")
_SECRET_NAME = re.compile(
    r"(?:TOKEN|SECRET|_KEY|DSN|_PASS|CLIENT_ID|_URL|ACCOUNT_ID|TEAM_ID|"
    r"KEY_ID|CAMPAIGN_ID|PRODUCT_ID|ORGANIZATION_ID|ADDRESS)$")
_ENV_READ = re.compile(
    r"\(\s*env\s*,\s*[\"'](?P<name>[A-Z][A-Z0-9_]+)[\"']")
_DEPLOY_ALLOWED = re.compile(r'local allowed="(?P<names>[^"]+)"')


class InventoryError(RuntimeError):
    """The manifest itself is malformed."""


@dataclass
class Finding:
    host: str
    locations: list[str] = field(default_factory=list)
    subresource: list[str] = field(default_factory=list)


# --------------------------------------------------------------------------
# Manifest


def load_manifest(path: Path = MANIFEST_PATH) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InventoryError(f"cannot read manifest: {exc}") from exc
    validate_manifest(manifest)
    return manifest


def _require_text(entry: dict, key: str, where: str) -> None:
    value = entry.get(key)
    if not isinstance(value, str) or not value.strip():
        raise InventoryError(f"{where}: {key} must be non-empty text")


def validate_manifest(manifest: dict) -> None:
    if manifest.get("schema") != SCHEMA_NAME:
        raise InventoryError("unsupported manifest schema")
    scope = manifest.get("inventoryScope")
    if not isinstance(scope, dict) or not scope.get("include"):
        raise InventoryError("inventoryScope.include is required")
    first_party = manifest.get("firstParty", {})
    if not first_party.get("suffixes"):
        raise InventoryError("firstParty.suffixes is required")

    ids: set[str] = set()
    hosts: dict[str, str] = {}
    for dep in manifest.get("dependencies", []):
        dep_id = dep.get("id")
        if not isinstance(dep_id, str) or dep_id in ids:
            raise InventoryError(f"dependency id missing or duplicated: {dep_id!r}")
        ids.add(dep_id)
        for key in ("name", "observes", "retention", "jurisdiction",
                    "canonicalData", "outage", "recovery", "alternative"):
            _require_text(dep, key, dep_id)
        if dep.get("necessity") not in NECESSITIES:
            raise InventoryError(f"{dep_id}: unknown necessity")
        categories = dep.get("categories")
        if not categories or any(c not in CATEGORIES for c in categories):
            raise InventoryError(f"{dep_id}: categories must use the vocabulary")
        if not dep.get("components"):
            raise InventoryError(f"{dep_id}: components are required")
        for host in [*dep.get("hosts", []), *dep.get("configuredHosts", [])]:
            if host in hosts:
                raise InventoryError(f"host {host} claimed by {hosts[host]} and {dep_id}")
            hosts[host] = dep_id
        if not dep.get("evidence"):
            raise InventoryError(f"{dep_id}: evidence is required")
        if not dep.get("drills") and not dep.get("gap"):
            raise InventoryError(f"{dep_id}: name a failure drill or record the gap")
    for ref in manifest.get("references", []):
        if ref.get("kind") not in REFERENCE_KINDS:
            raise InventoryError(f"reference {ref.get('host')}: unknown kind")
        if ref.get("host") in hosts:
            raise InventoryError(f"reference {ref['host']} is also a dependency host")
    for item in manifest.get("storedIdentifiers", []):
        if item.get("dependency") not in ids:
            raise InventoryError(f"stored identifier {item.get('table')}: unknown dependency")
        _require_text(item, "protection", item.get("table", "?"))


# --------------------------------------------------------------------------
# Scanning


def _is_ignorable_host(host: str) -> bool:
    if _IP_RE.match(host):
        return True
    if host.endswith(_RESERVED_SUFFIXES):
        return True
    return any(host == d or host.endswith("." + d) for d in _RESERVED_DOMAINS)


def is_first_party(host: str, manifest: dict) -> bool:
    return any(
        host == suffix or host.endswith("." + suffix)
        for suffix in manifest["firstParty"]["suffixes"]
    )


def _scoped_files(manifest: dict, root: Path) -> list[Path]:
    scope = manifest["inventoryScope"]
    excluded = [re.compile(pattern) for pattern in scope.get("exclude", [])]
    files: set[Path] = set()
    for pattern in scope["include"]:
        for path in root.glob(pattern):
            if not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if any(rule.search(relative) for rule in excluded):
                continue
            files.add(path)
    return sorted(files)


def scan_hosts(manifest: dict, root: Path = ROOT) -> dict[str, Finding]:
    findings: dict[str, Finding] = {}
    for path in _scoped_files(manifest, root):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        relative = path.relative_to(root).as_posix()
        for number, line in enumerate(text.splitlines(), start=1):
            if "://" not in line:
                continue
            for match in _HOST_RE.finditer(line):
                host = match.group("host").lower().rstrip(".")
                if _is_ignorable_host(host) or is_first_party(host, manifest):
                    continue
                finding = findings.setdefault(host, Finding(host))
                location = f"{relative}:{number}"
                finding.locations.append(location)
                prefix = line[:match.start()]
                if _SUBRESOURCE_PREFIX.search(prefix) or (
                        _LINK_TAG_FETCHES.search(line) and "href" in prefix[-12:]):
                    finding.subresource.append(location)
    return findings


def deploy_secrets(root: Path = ROOT) -> set[str]:
    text = (root / "app" / "deploy.sh").read_text(encoding="utf-8")
    match = _DEPLOY_ALLOWED.search(text)
    if not match:
        raise InventoryError("app/deploy.sh no longer declares its allowed secrets")
    return set(match.group("names").split())


def worker_env_credentials(root: Path = ROOT) -> set[str]:
    names: set[str] = set()
    for path in sorted((root / "app" / "src").glob("*.py")):
        for match in _ENV_READ.finditer(path.read_text(encoding="utf-8")):
            if _SECRET_NAME.search(match.group("name")):
                names.add(match.group("name"))
    return names


def schema_tables(root: Path = ROOT) -> dict[str, str]:
    """Map every D1 table to its CREATE statement, read without importing."""
    tree = ast.parse((root / "app" / "src" / "schema.py").read_text(encoding="utf-8"))
    statements: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant) and isinstance(node.value, str) \
                and "CREATE TABLE" in node.value:
            statements.append(node.value)
    tables: dict[str, str] = {}
    for statement in statements:
        for match in re.finditer(
                r"CREATE TABLE IF NOT EXISTS\s+(\w+)\s*\((.*?)\)\s*(?:;|$)",
                statement, re.S):
            tables.setdefault(match.group(1), match.group(2))
    return tables


def _test_exists(reference: str, root: Path) -> bool:
    path_text, _, name = reference.partition("::")
    path = root / path_text
    if not name or not path.is_file():
        return False
    name = name.split("[", 1)[0]
    source = path.read_text(encoding="utf-8")
    if path.suffix == ".py":
        return re.search(rf"^\s*(?:async\s+)?def {re.escape(name)}\(", source, re.M) is not None
    if path.suffix == ".go":
        return re.search(rf"^func {re.escape(name)}\(", source, re.M) is not None
    return f'"{name}"' in source or f"'{name}'" in source


# --------------------------------------------------------------------------
# Checking


def build_report(manifest: dict, root: Path = ROOT) -> dict:
    findings = scan_hosts(manifest, root)
    dependency_of: dict[str, str] = {}
    required_hosts: dict[str, str] = {}
    credentials: dict[str, str] = {}
    for dep in manifest["dependencies"]:
        for host in dep.get("hosts", []):
            dependency_of[host] = dep["id"]
            required_hosts[host] = dep["id"]
        for host in dep.get("configuredHosts", []):
            dependency_of[host] = dep["id"]
        for name in dep.get("credentials", []):
            credentials[name] = dep["id"]
    for name in manifest["firstParty"].get("secrets", []):
        credentials[name] = "first-party"
    references = {ref["host"]: ref for ref in manifest.get("references", [])}

    unclassified = {
        host: finding.locations[:5]
        for host, finding in sorted(findings.items())
        if host not in dependency_of and host not in references
    }
    fetched_references = {
        host: finding.subresource[:5]
        for host, finding in sorted(findings.items())
        if host in references and finding.subresource
    }
    stale_hosts = sorted(
        f"{dep_id}:{host}" for host, dep_id in required_hosts.items()
        if host not in findings
    )
    stale_references = sorted(host for host in references if host not in findings)

    secrets = deploy_secrets(root) | worker_env_credentials(root)
    unclaimed_secrets = sorted(secrets - set(credentials))

    tables = schema_tables(root)
    pattern = re.compile(manifest["storedIdentifierTablePattern"])
    claimed_tables = {item["table"] for item in manifest.get("storedIdentifiers", [])}
    unclaimed_tables = sorted(
        table for table in tables if pattern.search(table) and table not in claimed_tables)
    missing_identifier_columns = sorted(
        f"{item['table']}.{item['column']}"
        for item in manifest.get("storedIdentifiers", [])
        if item["table"] not in tables
        or not re.search(rf"\b{re.escape(item['column'])}\b", tables[item["table"]])
    )

    missing_evidence: list[str] = []
    missing_drills: list[str] = []
    for dep in manifest["dependencies"]:
        for evidence in dep["evidence"]:
            if not (root / evidence.split(":", 1)[0]).exists():
                missing_evidence.append(f"{dep['id']}:{evidence}")
        for drill in dep.get("drills", []):
            if not _test_exists(drill, root):
                missing_drills.append(f"{dep['id']}:{drill}")

    doc_path = root / manifest["documentation"]
    doc_current = doc_path.is_file() and extract_block(
        doc_path.read_text(encoding="utf-8")) == render_block(manifest)

    problems = {
        "unclassifiedHosts": unclassified,
        "referenceHostsFetchedAsSubresources": fetched_references,
        "manifestHostsNotFoundInSource": stale_hosts,
        "referenceHostsNotFoundInSource": stale_references,
        "unclaimedCredentials": unclaimed_secrets,
        "unclaimedIdentifierTables": unclaimed_tables,
        "missingIdentifierColumns": missing_identifier_columns,
        "missingEvidencePaths": missing_evidence,
        "missingDrillTests": missing_drills,
    }
    ok = not any(problems.values()) and doc_current
    necessity_counts = {n: 0 for n in NECESSITIES}
    for dep in manifest["dependencies"]:
        necessity_counts[dep["necessity"]] += 1
    return {
        "schema": "forkmesh-external-dependency-inventory-report-v1",
        "ok": ok,
        "documentationCurrent": doc_current,
        "measures": {
            "dependencies": len(manifest["dependencies"]),
            "undocumentedDependencies": len(unclassified) + len(unclaimed_secrets)
            + len(unclaimed_tables),
            "mandatoryHostedDependencies": necessity_counts["mandatory"],
            "dependenciesWithDrills": sum(
                1 for dep in manifest["dependencies"] if dep.get("drills")),
            "recordedGaps": sum(1 for dep in manifest["dependencies"] if dep.get("gap")),
            "outsideHostsFound": len(findings),
            "byNecessity": necessity_counts,
        },
        "problems": problems,
    }


# --------------------------------------------------------------------------
# Documentation


def _cell(text: str) -> str:
    return " ".join(str(text).split()).replace("|", "\\|")


NECESSITY_LABELS = {
    "mandatory": "Mandatory for the hosted service",
    "degradable": "Degrades one feature",
    "optional": "Optional, off unless enabled",
    "operator": "Operator tooling only",
    "build": "Build and release only",
}


def render_block(manifest: dict) -> str:
    deps = manifest["dependencies"]
    lines = [DOC_BEGIN, "", "<!-- Generated by tools/external_dependency_inventory.py render; edit the manifest, not this block. -->", ""]
    lines += ["## Summary", "",
              "| Service | Kind | Needed | Components | Observes |",
              "| --- | --- | --- | --- | --- |"]
    for dep in deps:
        lines.append(
            f"| [{_cell(dep['name'])}](#{dep['id']}) | {', '.join(dep['categories'])} "
            f"| {NECESSITY_LABELS[dep['necessity']]} | {', '.join(dep['components'])} "
            f"| {_cell(dep['observes'])} |")
    lines += ["", "## When each service disappears", "",
              "| Service | What still works / what stops | Self-host or offline alternative | Drill |",
              "| --- | --- | --- | --- |"]
    for dep in deps:
        drill = "tested" if dep.get("drills") else "**gap**"
        lines.append(
            f"| {_cell(dep['name'])} | {_cell(dep['outage'])} "
            f"| {_cell(dep['alternative'])} | {drill} |")
    lines += ["", "## Services in detail", ""]
    for dep in deps:
        lines += [f'<a id="{dep["id"]}"></a>', "", f"### {dep['name']}", "",
                  f"- **Needed:** {NECESSITY_LABELS[dep['necessity']]}",
                  f"- **Kind:** {', '.join(dep['categories'])}",
                  f"- **Components:** {', '.join(dep['components'])}"]
        hosts = [*dep.get("hosts", []), *dep.get("configuredHosts", [])]
        if hosts:
            lines.append("- **Hosts:** " + ", ".join(f"`{h}`" for h in hosts))
        if dep.get("credentials"):
            lines.append("- **Credentials:** " + ", ".join(f"`{c}`" for c in dep["credentials"]))
        lines += [
            f"- **Observes:** {_cell(dep['observes'])}",
            f"- **Retention:** {_cell(dep['retention'])}",
            f"- **Jurisdiction:** {_cell(dep['jurisdiction'])}",
            f"- **Canonical data:** {_cell(dep['canonicalData'])}",
            f"- **When it is gone:** {_cell(dep['outage'])}",
            f"- **Recovery:** {_cell(dep['recovery'])}",
            f"- **Alternative:** {_cell(dep['alternative'])}",
        ]
        if dep.get("drills"):
            lines.append("- **Failure drills:** " + ", ".join(f"`{d}`" for d in dep["drills"]))
        if dep.get("gap"):
            lines.append(f"- **Known gap:** {_cell(dep['gap'])}")
        lines.append("- **Evidence:** " + ", ".join(f"`{e}`" for e in dep["evidence"]))
        lines.append("")
    lines += ["## Stored third-party identifiers", "",
              "| Table | Column | Service | Protection |", "| --- | --- | --- | --- |"]
    for item in manifest.get("storedIdentifiers", []):
        lines.append(
            f"| `{item['table']}` | `{item['column']}` | {item['dependency']} "
            f"| {_cell(item['protection'])} |")
    lines += ["", "## Hosts that are named but never contacted automatically", "",
              "These appear in source as navigation links, share targets, schema "
              "namespaces, or documentation. The inventory fails if one is ever "
              "loaded as a script, image, stylesheet, frame, or fetch.", "",
              "| Host | Kind | Why it appears |", "| --- | --- | --- |"]
    for ref in manifest.get("references", []):
        lines.append(f"| `{ref['host']}` | {ref['kind']} | {_cell(ref['why'])} |")
    lines += ["", DOC_END]
    return "\n".join(lines)


def extract_block(text: str) -> str:
    start = text.find(DOC_BEGIN)
    end = text.find(DOC_END)
    if start < 0 or end < start:
        return ""
    return text[start:end + len(DOC_END)]


def render_document(manifest: dict, root: Path = ROOT) -> bool:
    path = root / manifest["documentation"]
    text = path.read_text(encoding="utf-8")
    current = extract_block(text)
    if not current:
        raise InventoryError(f"{manifest['documentation']} has no generated block markers")
    updated = text.replace(current, render_block(manifest))
    if updated != text:
        path.write_text(updated, encoding="utf-8")
        return True
    return False


# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("check", "report", "render"))
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        if args.command == "render":
            changed = render_document(manifest)
            print("external dependency map: " + ("documentation updated" if changed else "documentation current"))
            return 0
        report = build_report(manifest)
    except InventoryError as exc:
        print(f"external dependency map: {exc}", file=sys.stderr)
        return 2
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    if args.command == "report" or not report["ok"]:
        sys.stdout.write(encoded)
    else:
        print("external dependency map: inventory matches the manifest")
    return 0 if report["ok"] or args.command == "report" else 1


if __name__ == "__main__":
    raise SystemExit(main())
