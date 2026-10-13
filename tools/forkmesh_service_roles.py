#!/usr/bin/env python3
"""Service-role contract suite for independently self-hosted ForkMesh roles.

The v1 manifest in ``conformance/service-roles/v1/manifest.json`` (documented
in ``www/docs/operations/service-roles.md``) names every network role an
operator may run: its hosted implementation, its independent profile, the
HTTP contract that profile must answer, required and optional dependencies
with documented substitutes, default endpoints and their overrides, canonical
data, backup paths, observability, failure behavior, and remaining gaps.

* ``validate`` checks the manifest's structure and its parity with the
  repository: every path, source marker, override flag, and runbook anchor
  must exist, and a role may claim ``independent`` only without proprietary
  required dependencies or gaps.
* ``probe`` runs one role's contract against a running service and prints a
  non-secret report whose verdict is pass, degraded, fail, or unreachable.
* ``gaps`` prints each role's status and what still blocks independence.

Standard library only, so an operator can run it beside a fresh deployment.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
import time
from typing import Any, Mapping
from urllib import error as urlerror
from urllib import request as urlrequest
from urllib.parse import quote, urlsplit, urlunsplit


TOOL_VERSION = "1.0.0"
ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "conformance" / "service-roles" / "v1" / "manifest.json"
REPORT_SCHEMA = "forkmesh-service-role-report-v1"
USER_AGENT = "forkmesh-service-roles/%s (+https://forkmesh.com/docs)" % TOOL_VERSION

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_DEGRADED = 3
EXIT_UNREACHABLE = 4

# The eight roles issue #502 requires to be independently self-hostable.
REQUIRED_ROLES = (
    "web-ui", "relay", "public-index", "search", "identity-resolver",
    "collaboration-relay", "automation-broker", "organization-service",
)
STATUSES = ("independent", "partial", "hosted-only")
VERDICTS = ("pass", "degraded", "fail", "unreachable")
JSON_TYPES = {
    "bool": bool, "int": int, "str": str, "list": list, "dict": dict,
}
PROFILE_FIELDS = (
    "implementation", "paths", "start", "check", "configSchema", "runbook",
    "evidence",
)
ROLE_FIELDS = (
    "id", "label", "status", "summary", "hosted", "contract", "dependencies",
    "defaultEndpoints", "canonicalData", "backup", "observability", "failure",
    "gaps",
)
FORKMESH_HOST_RE = re.compile(r"(^|\.)forkmesh\.com$", re.IGNORECASE)


class ManifestError(ValueError):
    """The service-role manifest is malformed or disagrees with the repository."""


def load_manifest(path: str | os.PathLike | None = None) -> dict:
    target = Path(path) if path is not None else MANIFEST_PATH
    try:
        value = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError("cannot read service-role manifest: %s" % exc) from exc
    if not isinstance(value, dict):
        raise ManifestError("service-role manifest must be a JSON object")
    return value


def heading_anchor(heading: str) -> str:
    """GitHub-style heading slug, as the docs site renders it."""
    text = re.sub(r"[`*_]", "", heading.strip().lower())
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def _doc_reference_errors(label: str, reference: Any, root: Path) -> list[str]:
    if not isinstance(reference, str) or not reference:
        return ["%s: must be a repository path" % label]
    path_text, _, anchor = reference.partition("#")
    path = root / path_text
    if not path.is_file():
        return ["%s: %s does not exist" % (label, path_text)]
    if not anchor:
        return []
    anchors = {
        heading_anchor(line.lstrip("#"))
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.startswith("#")
    }
    if anchor not in anchors:
        return ["%s: %s has no heading #%s" % (label, path_text, anchor)]
    return []


def _path_errors(label: str, paths: Any, root: Path) -> list[str]:
    if not isinstance(paths, list) or not paths:
        return ["%s: must list at least one path" % label]
    return [
        "%s: %s does not exist" % (label, item)
        for item in paths
        if not isinstance(item, str) or not (root / item).exists()
    ]


def _marker_errors(label: str, source: Any, root: Path) -> list[str]:
    if not isinstance(source, dict):
        return ["%s: source must name a path and marker" % label]
    path, marker = source.get("path"), source.get("marker")
    if not isinstance(path, str) or not isinstance(marker, str) or not marker:
        return ["%s: source must name a path and marker" % label]
    target = root / path
    if not target.is_file():
        return ["%s: %s does not exist" % (label, path)]
    if marker not in target.read_text(encoding="utf-8"):
        return ["%s: %s no longer contains %r" % (label, path, marker)]
    return []


def _expectation_errors(label: str, expect: Any) -> list[str]:
    if not isinstance(expect, dict):
        return ["%s: must be an object" % label]
    errors = []
    statuses = expect.get("status")
    if (not isinstance(statuses, list) or not statuses
            or not all(isinstance(code, int) and 100 <= code <= 599
                       for code in statuses)):
        errors.append("%s: status must list HTTP status codes" % label)
    fields = expect.get("json", {})
    if not isinstance(fields, dict) or any(
            kind not in JSON_TYPES for kind in fields.values()):
        errors.append("%s: json field types must be one of %s"
                      % (label, ", ".join(sorted(JSON_TYPES))))
    if "contentType" in expect and not isinstance(expect["contentType"], str):
        errors.append("%s: contentType must be a string" % label)
    return errors


def manifest_errors(manifest: Mapping[str, Any], root: Path = ROOT) -> list[str]:
    errors: list[str] = []
    if manifest.get("schemaVersion") != 1:
        errors.append("schemaVersion must be 1")
    suite = manifest.get("suite")
    if not isinstance(suite, dict) or suite.get("reportSchema") != REPORT_SCHEMA:
        errors.append("suite.reportSchema must be %s" % REPORT_SCHEMA)
    else:
        for key in ("tool", "documentation"):
            errors.extend(_doc_reference_errors("suite.%s" % key,
                                                suite.get(key), root))
    if set(manifest.get("statuses") or {}) != set(STATUSES):
        errors.append("statuses must define exactly %s" % ", ".join(STATUSES))
    if set(manifest.get("failureVerdicts") or {}) != set(VERDICTS):
        errors.append("failureVerdicts must define exactly %s"
                      % ", ".join(VERDICTS))

    roles = manifest.get("roles")
    if not isinstance(roles, list):
        return errors + ["roles must be a list"]
    seen: list[str] = []
    for index, role in enumerate(roles):
        if not isinstance(role, dict):
            errors.append("roles[%d]: must be an object" % index)
            continue
        role_id = role.get("id")
        label = "role %s" % role_id
        if role_id not in REQUIRED_ROLES:
            errors.append("roles[%d]: unknown role %r" % (index, role_id))
            continue
        if role_id in seen:
            errors.append("%s: duplicate" % label)
        seen.append(role_id)
        missing = [key for key in ROLE_FIELDS if key not in role]
        if missing:
            errors.append("%s: missing %s" % (label, ", ".join(missing)))
            continue
        errors.extend(_role_errors(label, role, root))
    for role_id in REQUIRED_ROLES:
        if role_id not in seen:
            errors.append("role %s: not declared" % role_id)
    return errors


def _role_errors(label: str, role: Mapping[str, Any], root: Path) -> list[str]:
    errors: list[str] = []
    status = role["status"]
    if status not in STATUSES:
        errors.append("%s: unknown status %r" % (label, status))

    hosted = role["hosted"]
    if not isinstance(hosted, dict) or not hosted.get("implementation"):
        errors.append("%s: hosted must name its implementation" % label)
    else:
        errors.extend(_path_errors(label + " hosted.paths",
                                   hosted.get("paths"), root))

    profile = role.get("independentProfile")
    if profile is not None:
        missing = [key for key in PROFILE_FIELDS if not profile.get(key)]
        if missing:
            errors.append("%s: independentProfile missing %s"
                          % (label, ", ".join(missing)))
        else:
            errors.extend(_path_errors(label + " independentProfile.paths",
                                       profile["paths"], root))
            errors.extend(_path_errors(label + " independentProfile.evidence",
                                       profile["evidence"], root))
            errors.extend(_doc_reference_errors(
                label + " configSchema", profile["configSchema"], root))
            errors.extend(_doc_reference_errors(
                label + " runbook", profile["runbook"], root))
            for key in ("start", "check"):
                if not all(isinstance(part, str) and part
                           for part in profile[key]):
                    errors.append("%s: independentProfile.%s must be an "
                                  "argument array" % (label, key))
    elif status != "hosted-only":
        errors.append("%s: a %s role needs an independentProfile"
                      % (label, status))

    contract = role["contract"]
    if not isinstance(contract, list):
        errors.append("%s: contract must be a list" % label)
        contract = []
    check_ids = set()
    for check in contract:
        check_label = "%s check %s" % (label, check.get("id"))
        if not check.get("id") or check["id"] in check_ids:
            errors.append("%s: needs a unique id" % check_label)
        check_ids.add(check.get("id"))
        if check.get("method") not in ("GET", "HEAD"):
            errors.append("%s: probes are read-only GET or HEAD" % check_label)
        if not str(check.get("path", "")).startswith("/"):
            errors.append("%s: path must start with /" % check_label)
        errors.extend(_expectation_errors(check_label + " expect",
                                          check.get("expect")))
        if "degraded" in check:
            errors.extend(_expectation_errors(check_label + " degraded",
                                              check["degraded"]))
        errors.extend(_marker_errors(check_label, check.get("source"), root))

    dependencies = role["dependencies"]
    required = dependencies.get("required") if isinstance(dependencies, dict) else None
    optional = dependencies.get("optional") if isinstance(dependencies, dict) else None
    if not isinstance(required, list) or not isinstance(optional, list):
        errors.append("%s: dependencies need required and optional lists" % label)
        required, optional = [], []
    for dependency in required + optional:
        if not dependency.get("id") or not isinstance(
                dependency.get("proprietary"), bool):
            errors.append("%s: every dependency needs an id and a proprietary "
                          "flag" % label)
    for dependency in optional:
        dep_label = "%s optional dependency %s" % (label, dependency.get("id"))
        if not dependency.get("substitute"):
            errors.append("%s: needs a documented substitute" % dep_label)
        errors.extend(_doc_reference_errors(dep_label + " docs",
                                            dependency.get("docs"), root))

    for endpoint in role["defaultEndpoints"]:
        value = str(endpoint.get("value", ""))
        endpoint_label = "%s default endpoint %s" % (label, value)
        override = endpoint.get("override")
        if not isinstance(override, dict) or not override.get("name"):
            errors.append("%s: needs an override" % endpoint_label)
            continue
        errors.extend(_marker_errors(endpoint_label + " override", override,
                                     root))

    for key in ("canonicalData", "failure", "summary", "label"):
        if not isinstance(role[key], str) or not role[key].strip():
            errors.append("%s: %s must be a sentence" % (label, key))
    if not isinstance(role["observability"], list) or not role["observability"]:
        errors.append("%s: observability must list at least one surface" % label)
    backup = role["backup"]
    if not isinstance(backup, dict) or not backup.get("paths"):
        errors.append("%s: backup must list paths" % label)
    else:
        errors.extend(_doc_reference_errors(label + " backup.runbook",
                                            backup.get("runbook"), root))

    gaps = role["gaps"]
    if not isinstance(gaps, list) or not all(
            isinstance(gap, str) and gap for gap in gaps):
        errors.append("%s: gaps must be a list of sentences" % label)
        gaps = []
    if status == "independent":
        if gaps:
            errors.append("%s: an independent role cannot list gaps" % label)
        if any(dependency.get("proprietary") for dependency in required):
            errors.append("%s: an independent role cannot require a "
                          "proprietary dependency" % label)
        if not contract:
            errors.append("%s: an independent role needs a contract" % label)
    elif not gaps:
        errors.append("%s: a %s role must list its gaps" % (label, status))
    return errors


def validate_manifest(manifest: Mapping[str, Any], root: Path = ROOT) -> None:
    errors = manifest_errors(manifest, root)
    if errors:
        raise ManifestError("; ".join(errors))


def find_role(manifest: Mapping[str, Any], role_id: str) -> dict:
    for role in manifest.get("roles") or []:
        if role.get("id") == role_id:
            return role
    raise ManifestError("unknown role %r" % role_id)


# ------------------------------------------------------------------- probing


def redact_url(url: str) -> str:
    parts = urlsplit(url)
    host = parts.hostname or ""
    if parts.port:
        host = "%s:%d" % (host, parts.port)
    return urlunsplit((parts.scheme, host, parts.path, "", ""))


def expand_path(template: str, params: Mapping[str, str]) -> str:
    def replace(match: re.Match) -> str:
        name = match.group(1)
        if name not in params:
            raise ManifestError("missing --param %s for %s" % (name, template))
        return quote(params[name], safe="")
    return re.sub(r"\{([A-Za-z]+)\}", replace, template)


def _matches(expect: Mapping[str, Any], status: int, content_type: str,
             payload: Any) -> list[str]:
    problems = []
    if status not in expect["status"]:
        problems.append("status %d not in %s" % (status, expect["status"]))
    wanted_type = expect.get("contentType")
    if wanted_type and not content_type.startswith(wanted_type):
        problems.append("content type %r is not %s" % (content_type, wanted_type))
    fields = expect.get("json") or {}
    if fields:
        if not isinstance(payload, dict):
            problems.append("body is not a JSON object")
        else:
            for name, kind in fields.items():
                value = payload.get(name)
                expected = JSON_TYPES[kind]
                if value is None or not isinstance(value, expected) or (
                        expected is int and isinstance(value, bool)):
                    problems.append("field %s is not %s" % (name, kind))
    return problems


def fetch(url: str, method: str, timeout: float) -> tuple[int, str, bytes]:
    req = urlrequest.Request(url, method=method,
                             headers={"User-Agent": USER_AGENT,
                                      "Accept": "*/*"})
    try:
        with urlrequest.urlopen(req, timeout=timeout) as response:
            return (response.status, response.headers.get("Content-Type", ""),
                    response.read(1 << 20))
    except urlerror.HTTPError as exc:
        return exc.code, exc.headers.get("Content-Type", ""), exc.read(1 << 20)


def probe_role(manifest: Mapping[str, Any], role_id: str, base_url: str, *,
               params: Mapping[str, str] | None = None, timeout: float = 10.0,
               fetcher=fetch) -> dict:
    role = find_role(manifest, role_id)
    base = base_url.rstrip("/")
    results = []
    telemetry = {"requests": 0, "transportErrors": 0, "statusClasses": {},
                 "latencyMs": 0}
    started = time.monotonic()
    for check in role["contract"]:
        path = expand_path(check["path"], params or {})
        entry = {"id": check["id"], "method": check["method"],
                 "path": path.split("?", 1)[0]}
        telemetry["requests"] += 1
        try:
            status, content_type, body = fetcher(base + path, check["method"],
                                                 timeout)
        except (OSError, urlerror.URLError) as exc:
            telemetry["transportErrors"] += 1
            entry.update(result="unreachable",
                         detail=type(getattr(exc, "reason", exc)).__name__)
            results.append(entry)
            continue
        status_class = "%dxx" % (status // 100)
        telemetry["statusClasses"][status_class] = (
            telemetry["statusClasses"].get(status_class, 0) + 1)
        try:
            payload = json.loads(body.decode("utf-8")) if body else None
        except (UnicodeDecodeError, json.JSONDecodeError):
            payload = None
        entry["status"] = status
        problems = _matches(check["expect"], status, content_type, payload)
        if not problems:
            entry["result"] = "pass"
        elif "degraded" in check and not _matches(
                check["degraded"], status, content_type, payload):
            entry["result"] = "degraded"
            if isinstance(payload, dict) and isinstance(payload.get("error"), str):
                entry["error"] = payload["error"][:120]
        else:
            entry.update(result="fail", problems=problems)
        results.append(entry)
    telemetry["latencyMs"] = int((time.monotonic() - started) * 1000)
    outcomes = {entry["result"] for entry in results}
    if not results:
        verdict = "fail"
    elif outcomes == {"unreachable"}:
        verdict = "unreachable"
    elif "fail" in outcomes or "unreachable" in outcomes:
        verdict = "fail"
    elif "degraded" in outcomes:
        verdict = "degraded"
    else:
        verdict = "pass"
    return {
        "schema": REPORT_SCHEMA,
        "tool": TOOL_VERSION,
        "suiteVersion": manifest["suite"]["version"],
        "role": role_id,
        "roleStatus": role["status"],
        "baseUrl": redact_url(base_url),
        "defaultPublicService": bool(
            FORKMESH_HOST_RE.search(urlsplit(base_url).hostname or "")),
        "verdict": verdict,
        "checks": results,
        "telemetry": telemetry,
    }


VERDICT_EXIT = {
    "pass": EXIT_OK, "degraded": EXIT_DEGRADED, "fail": EXIT_FAILED,
    "unreachable": EXIT_UNREACHABLE,
}


def gap_summary(manifest: Mapping[str, Any]) -> dict:
    roles = [
        {"id": role["id"], "status": role["status"], "gaps": role["gaps"],
         "contractChecks": len(role["contract"])}
        for role in manifest["roles"]
    ]
    counts = {status: sum(1 for role in roles if role["status"] == status)
              for status in STATUSES}
    return {"schema": "forkmesh-service-role-gaps-v1",
            "suiteVersion": manifest["suite"]["version"],
            "counts": counts, "roles": roles}


# ----------------------------------------------------------------------- CLI


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--manifest", default=str(MANIFEST_PATH))
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="check the manifest against the repository")
    probe = commands.add_parser("probe", help="run one role's contract")
    probe.add_argument("--role", required=True, choices=REQUIRED_ROLES)
    probe.add_argument("--base-url", required=True)
    probe.add_argument("--param", action="append", default=[],
                       metavar="NAME=VALUE",
                       help="path template value, e.g. owner=forkmesh")
    probe.add_argument("--timeout", type=float, default=10.0)
    gaps = commands.add_parser("gaps", help="print role status and gaps")
    gaps.add_argument("--json", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        if args.command == "validate":
            validate_manifest(manifest)
            print("service-role manifest is valid: %d roles"
                  % len(manifest["roles"]))
            return EXIT_OK
        validate_manifest(manifest)
        if args.command == "gaps":
            summary = gap_summary(manifest)
            if args.json:
                print(json.dumps(summary, indent=2, sort_keys=True))
            else:
                for role in summary["roles"]:
                    print("%-22s %s" % (role["id"], role["status"]))
                    for gap in role["gaps"]:
                        print("  - " + gap)
            return EXIT_OK
        params = {}
        for item in args.param:
            name, separator, value = item.partition("=")
            if not separator or not name:
                raise ManifestError("--param must be NAME=VALUE")
            params[name] = value
        report = probe_role(manifest, args.role, args.base_url, params=params,
                            timeout=args.timeout)
        print(json.dumps(report, indent=2, sort_keys=True))
        return VERDICT_EXIT[report["verdict"]]
    except ManifestError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
