#!/usr/bin/env python3
"""Run and publish ForkMesh's versioned stock-Git compatibility contract.

The offline probe starts disposable smart-HTTP origins plus relay and failover
proxies.  It invokes an unmodified Git executable for every user operation and
emits a non-secret JSON report containing versions, generalized timings,
operation results, and object/ref integrity evidence.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from datetime import date, datetime, timezone
import hashlib
import html
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from typing import Iterable, Iterator
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_DIRECTORY = ROOT / "conformance" / "git" / "v1"
MATRIX_PATH = CONTRACT_DIRECTORY / "matrix.json"
RESULTS_PATH = CONTRACT_DIRECTORY / "results.json"
EVIDENCE_DIRECTORY = CONTRACT_DIRECTORY / "evidence"
MARKDOWN_PATH = ROOT / "www" / "docs" / "git-compatibility.md"
PUBLIC_DIRECTORY = ROOT / "www" / "public" / "docs" / "git-compatibility"
PUBLIC_HTML_PATH = PUBLIC_DIRECTORY / "index.html"
PUBLIC_JSON_PATH = PUBLIC_DIRECTORY / "matrix.json"
PUBLIC_RESULTS_PATH = PUBLIC_DIRECTORY / "results.json"
PUBLIC_EVIDENCE_DIRECTORY = PUBLIC_DIRECTORY / "evidence"
REPORT_SCHEMA = "forkmesh-git-conformance-report-v1"
RESULTS_SCHEMA = "forkmesh-git-conformance-results-v1"
PUBLISHED_SCHEMA = "forkmesh-git-compatibility-published-v1"
REQUIRED_OPERATIONS = {
    "clone",
    "fetch",
    "push",
    "hooks",
    "branches",
    "rebase",
    "force-with-lease",
    "force",
    "shallow-clone",
    "partial-clone",
    "bundle",
    "submodules",
    "signed-commits",
    "integrity",
    "rollback-recovery",
}
REQUIRED_PATHS = {"direct", "relay", "mirror-failover"}
RETRYABLE_PROXY_STATUSES = {429, 502, 503, 504}
# Matrix statuses that promise a workflow works. Each one must be backed by a
# passing, fresh run or it is published as one of DEGRADED_STATUSES instead.
CLAIM_STATUSES = ("supported", "local", "conditional")
DEGRADED_STATUSES = {
    "failing": "A retained run for this cell failed; the claim is withdrawn until the same configuration passes again.",
    "stale": "The newest passing run is older than the freshness window; re-run the suite before relying on it.",
    "unverified": "No retained run covers this cell; treat it as experimental.",
}
REPORT_ROW_STATUSES = {"pass", "fail", "not-run", "running"}
PRIVACY_FIELDS = (
    "temporaryPathsIncluded",
    "repositoryContentsIncluded",
    "credentialsIncluded",
    "clientAddressesIncluded",
)
# Evidence is published verbatim, so anything the runner's redaction missed
# must stop attestation rather than reach the public docs.
EVIDENCE_LEAK = re.compile(
    r"file://|127\.0\.0\.1|\[::1\]|[/\\]forkmesh-git-conformance-"
    r"|/home/|/Users/|[A-Za-z]:\\\\Users\\\\"
)
ISO_DATE = re.compile(r"20[0-9]{2}-[01][0-9]-[0-3][0-9]")


class ConformanceError(RuntimeError):
    """A contract, fixture, Git command, or integrity assertion failed."""


def _display(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def _json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ConformanceError(f"cannot read {_display(path)}: {exc}") from exc
    if not isinstance(value, dict):
        raise ConformanceError(f"{_display(path)} must be a JSON object")
    return value


def load_matrix() -> dict:
    return _json(MATRIX_PATH)


def _command(value: object, field: str) -> list[str]:
    if (
        not isinstance(value, list)
        or not value
        or not all(isinstance(part, str) and part for part in value)
    ):
        raise ConformanceError(f"{field} must be a non-empty argument array")
    return list(value)


def _iso_date(value: object, field: str) -> date:
    text = str(value or "")
    try:
        if ISO_DATE.fullmatch(text):
            return date.fromisoformat(text)
    except ValueError:
        pass
    raise ConformanceError(f"{field} must be an ISO date")


def validate_matrix(matrix: dict) -> None:
    if matrix.get("schemaVersion") != 1:
        raise ConformanceError("unsupported compatibility matrix schemaVersion")
    suite = matrix.get("suite")
    if not isinstance(suite, dict) or suite.get("id") != "forkmesh-stock-git":
        raise ConformanceError("matrix suite id must be forkmesh-stock-git")
    if not re.fullmatch(r"[1-9][0-9]*\.[0-9]+\.[0-9]+", str(suite.get("version") or "")):
        raise ConformanceError("suite.version must be a semantic version")
    if suite.get("reportSchema") != REPORT_SCHEMA:
        raise ConformanceError("suite.reportSchema does not match the runner")
    _iso_date(suite.get("updated"), "suite.updated")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", str(suite.get("minimumGitVersion") or "")):
        raise ConformanceError("suite.minimumGitVersion must be x.y.z")
    freshness = suite.get("freshnessDays")
    if type(freshness) is not int or not 1 <= freshness <= 365:
        raise ConformanceError("suite.freshnessDays must be an integer from 1 to 365")
    if not str(suite.get("fixture") or "").strip():
        raise ConformanceError("suite.fixture must describe the tested repository shape")

    statuses = matrix.get("statuses")
    if not isinstance(statuses, dict) or set(statuses) != {
        "supported", "local", "conditional", "unsupported"
    }:
        raise ConformanceError("matrix statuses are incomplete")
    paths = matrix.get("paths")
    if not isinstance(paths, list):
        raise ConformanceError("matrix paths must be a list")
    path_ids = [item.get("id") for item in paths if isinstance(item, dict)]
    if set(path_ids) != REQUIRED_PATHS or len(path_ids) != len(REQUIRED_PATHS):
        raise ConformanceError("matrix must define direct, relay, and mirror-failover once")
    for item in paths:
        if not isinstance(item.get("read"), bool) or not isinstance(item.get("write"), bool):
            raise ConformanceError(f"path {item.get('id')} needs read/write booleans")
        for field in ("label", "description", "transport", "authentication"):
            if not str(item.get(field) or "").strip():
                raise ConformanceError(f"path {item.get('id')} needs a {field}")

    limitations = matrix.get("limitations")
    if not isinstance(limitations, list) or not limitations:
        raise ConformanceError("matrix limitations must be a non-empty list")
    limitation_ids: set[str] = set()
    for item in limitations:
        if not isinstance(item, dict) or not re.fullmatch(
            r"[a-z0-9]+(?:-[a-z0-9]+)*", str(item.get("id") or "")
        ):
            raise ConformanceError("every limitation needs a stable kebab-case id")
        limitation_id = item["id"]
        if limitation_id in limitation_ids:
            raise ConformanceError(f"duplicate limitation: {limitation_id}")
        limitation_ids.add(limitation_id)
        if not set(item.get("appliesTo") or []).issubset(REQUIRED_PATHS):
            raise ConformanceError(f"{limitation_id}: invalid appliesTo path")
        for field in ("title", "expected", "action", "recovery"):
            if not str(item.get(field) or "").strip():
                raise ConformanceError(f"{limitation_id}: missing {field}")
        _command(item.get("reproduction"), f"{limitation_id}.reproduction")
        tested = item.get("lastTested")
        if not isinstance(tested, dict) or not all(
            str(tested.get(field) or "").strip()
            for field in ("date", "client", "evidence")
        ):
            raise ConformanceError(f"{limitation_id}: lastTested is incomplete")

    operations = matrix.get("operations")
    if not isinstance(operations, list):
        raise ConformanceError("matrix operations must be a list")
    operation_ids = [item.get("id") for item in operations if isinstance(item, dict)]
    probes = [item.get("probe") for item in operations if isinstance(item, dict)]
    if set(operation_ids) != REQUIRED_OPERATIONS or len(operation_ids) != len(REQUIRED_OPERATIONS):
        raise ConformanceError("matrix operation inventory is incomplete or duplicated")
    if set(probes) != REQUIRED_OPERATIONS or len(probes) != len(REQUIRED_OPERATIONS):
        raise ConformanceError("every promised operation needs one executable probe")
    for operation in operations:
        operation_id = operation["id"]
        _command(operation.get("reproduction"), f"{operation_id}.reproduction")
        support = operation.get("support")
        if not isinstance(support, dict) or set(support) != REQUIRED_PATHS:
            raise ConformanceError(f"{operation_id}: support must cover every path")
        for path_id, cell in support.items():
            if not isinstance(cell, dict) or cell.get("status") not in statuses:
                raise ConformanceError(f"{operation_id}/{path_id}: invalid status")
            if not str(cell.get("note") or "").strip():
                raise ConformanceError(f"{operation_id}/{path_id}: missing note")
            if cell["status"] in {"conditional", "unsupported"}:
                if cell.get("limitation") not in limitation_ids:
                    raise ConformanceError(
                        f"{operation_id}/{path_id}: status needs a known limitation"
                    )

    clients = matrix.get("clients")
    if not isinstance(clients, list) or not clients:
        raise ConformanceError("matrix clients must be a non-empty list")
    client_ids: set[str] = set()
    for client in clients:
        if not isinstance(client, dict) or not str(client.get("id") or ""):
            raise ConformanceError("every client needs an id")
        if client["id"] in client_ids:
            raise ConformanceError(f"duplicate client: {client['id']}")
        client_ids.add(client["id"])
        for field in ("name", "platform", "version", "status", "lastTested", "evidence"):
            if not str(client.get(field) or "").strip():
                raise ConformanceError(f"{client['id']}: missing {field}")
        if client["status"] == "tested":
            # A hand-recorded certification can only age out if it is dated.
            _iso_date(client["lastTested"], f"{client['id']}.lastTested")
    expected_desktop = {"github-desktop", "sourcetree", "gitkraken"}
    if not expected_desktop.issubset(client_ids):
        raise ConformanceError("common third-party desktop clients are missing")


def _finished(report: dict) -> datetime:
    try:
        finished = datetime.fromisoformat(str(report.get("finishedAt") or ""))
    except ValueError as exc:
        raise ConformanceError("report finishedAt must be an ISO timestamp") from exc
    if finished.tzinfo is None:
        raise ConformanceError("report finishedAt must include a UTC offset")
    return finished.astimezone(timezone.utc)


def report_id(report: dict) -> str:
    """Content address of a report; editing retained evidence changes its id."""
    canonical = json.dumps(
        report, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:16]


def _evidence_text(report: dict) -> str:
    return json.dumps(report, indent=2, sort_keys=True) + "\n"


def _probe_paths(matrix: dict) -> dict[str, list[str]]:
    return {
        operation["probe"]: [
            path["id"] for path in matrix["paths"]
            if operation["support"][path["id"]]["status"] != "unsupported"
        ]
        for operation in matrix["operations"]
    }


def validate_report(matrix: dict, report: object) -> None:
    """Accept a runner report as public evidence, passing or failing."""
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise ConformanceError("evidence is not a stock-Git conformance report")
    suite = report.get("suite")
    if not isinstance(suite, dict) or suite.get("id") != matrix["suite"]["id"]:
        raise ConformanceError("report belongs to a different conformance suite")
    if suite.get("version") != matrix["suite"]["version"]:
        raise ConformanceError(
            f"report was produced by suite {suite.get('version')}; re-run suite "
            f"{matrix['suite']['version']}"
        )
    if not re.fullmatch(r"[0-9a-f]{40}", str(report.get("testedCommit") or "")):
        raise ConformanceError("report testedCommit must be a full commit id")
    _finished(report)
    client = report.get("client")
    if not isinstance(client, dict):
        raise ConformanceError("report client is missing")
    for field in ("label", "gitVersion", "platform", "platformRelease", "machine"):
        if not isinstance(client.get(field), str) or not client[field].strip():
            raise ConformanceError(f"report client.{field} is missing")
    if client.get("protocolVersion") not in {"0", "2"}:
        raise ConformanceError("report client.protocolVersion must be 0 or 2")
    if client.get("transportMode") not in {"http", "file"}:
        raise ConformanceError("report client.transportMode must be http or file")
    rows = report.get("operations")
    expected_paths = _probe_paths(matrix)
    if not isinstance(rows, list) or not all(isinstance(row, dict) for row in rows):
        raise ConformanceError("report operations must be a list")
    row_ids = [row.get("id") for row in rows]
    if set(row_ids) != set(expected_paths) or len(row_ids) != len(expected_paths):
        raise ConformanceError("report operation inventory does not match the matrix")
    for row in rows:
        if row.get("status") not in REPORT_ROW_STATUSES:
            raise ConformanceError(f"report {row['id']}: invalid status")
        if row.get("paths") != expected_paths[row["id"]]:
            raise ConformanceError(f"report {row['id']}: paths do not match the matrix")
        if not isinstance(row.get("detail"), str):
            raise ConformanceError(f"report {row['id']}: detail must be text")
    summary = report.get("summary")
    passed = all(row["status"] == "pass" for row in rows)
    if not isinstance(summary, dict) or summary.get("status") != (
        "pass" if passed else "fail"
    ):
        raise ConformanceError("report summary disagrees with its operation rows")
    integrity = report.get("integrity")
    if not isinstance(integrity, dict) or any(
        integrity.get(field) is not False for field in PRIVACY_FIELDS
    ):
        raise ConformanceError("report does not attest that private data was excluded")
    if EVIDENCE_LEAK.search(json.dumps(report)):
        raise ConformanceError(
            "report contains an unredacted path, file URL, or loopback address"
        )


def _configuration(report: dict) -> tuple[str, ...]:
    client = report["client"]
    return tuple(
        client[field]
        for field in (
            "label", "gitVersion", "platform", "machine",
            "protocolVersion", "transportMode",
        )
    )


def _run_entry(run_id: str, report: dict) -> dict:
    client = report["client"]
    rows = report["operations"]
    passed = sum(row["status"] == "pass" for row in rows)
    failed = sum(row["status"] == "fail" for row in rows)
    return {
        "id": run_id,
        "evidence": f"evidence/{run_id}.json",
        "finishedAt": report["finishedAt"],
        "date": _finished(report).date().isoformat(),
        "testedCommit": report["testedCommit"],
        "client": {
            field: client[field]
            for field in (
                "label", "gitVersion", "platform", "platformRelease", "machine",
                "protocolVersion", "transportMode",
            )
        },
        "summary": {
            "status": report["summary"]["status"],
            "pass": passed,
            "fail": failed,
            "notRun": len(rows) - passed - failed,
        },
        "reproduction": {
            "checkout": ["git", "checkout", report["testedCommit"]],
            "command": [
                "python3", "tools/git_conformance.py", "run",
                "--protocol-version", client["protocolVersion"],
                "--transport", client["transportMode"],
                "--client-label", client["label"],
                "--output", f"artifacts/git-conformance-v{client['protocolVersion']}.json",
            ],
        },
    }


def _ordered(reports: dict[str, dict]) -> list[tuple[str, dict]]:
    return sorted(
        reports.items(), key=lambda item: (_finished(item[1]), item[0]), reverse=True
    )


def results_document(matrix: dict, reports: dict[str, dict], attested: str) -> dict:
    """The committed ledger is fully derived from retained evidence."""
    return {
        "schema": RESULTS_SCHEMA,
        "suite": {
            "id": matrix["suite"]["id"],
            "version": matrix["suite"]["version"],
        },
        "attested": attested,
        "runs": [_run_entry(run_id, report) for run_id, report in _ordered(reports)],
    }


def load_evidence(matrix: dict) -> tuple[dict[str, dict], str]:
    if not RESULTS_PATH.exists():
        if EVIDENCE_DIRECTORY.exists() and any(EVIDENCE_DIRECTORY.iterdir()):
            raise ConformanceError(
                "retained evidence exists without conformance/git/v1/results.json"
            )
        return {}, matrix["suite"]["updated"]
    ledger = _json(RESULTS_PATH)
    if ledger.get("schema") != RESULTS_SCHEMA:
        raise ConformanceError("results.json has an unsupported schema")
    attested = _iso_date(ledger.get("attested"), "results.attested").isoformat()
    reports: dict[str, dict] = {}
    if EVIDENCE_DIRECTORY.exists():
        for path in sorted(EVIDENCE_DIRECTORY.iterdir()):
            report = _json(path)
            run_id = report_id(report)
            if path.name != f"{run_id}.json":
                raise ConformanceError(
                    f"{_display(path)} was edited after attestation; re-run and attest instead"
                )
            reports[run_id] = report
    return reports, attested


def validate_evidence(matrix: dict, reports: dict[str, dict], attested: str) -> None:
    configurations: set[tuple[str, ...]] = set()
    newest = None
    for report in reports.values():
        validate_report(matrix, report)
        configuration = _configuration(report)
        if configuration in configurations:
            raise ConformanceError(
                "two retained runs share one client configuration: " + " / ".join(configuration)
            )
        configurations.add(configuration)
        finished = _finished(report).date()
        newest = finished if newest is None or finished > newest else newest
    if newest is not None and newest > date.fromisoformat(attested):
        raise ConformanceError("results.attested predates a retained run")
    expected = results_document(matrix, reports, attested)
    actual = _json(RESULTS_PATH) if RESULTS_PATH.exists() else expected
    if actual != expected:
        raise ConformanceError(
            "conformance/git/v1/results.json does not match the retained evidence; "
            "run python3 tools/git_conformance.py attest"
        )


def attest(
    matrix: dict,
    reports: dict[str, dict],
    new_reports: Iterable[dict],
    *,
    drop: Iterable[str] = (),
) -> dict[str, dict]:
    """Merge runner reports into the retained evidence set.

    Failing reports are accepted on purpose: that is how a regression reaches
    the published matrix. The newest run per client configuration replaces
    older ones, so re-running the same configuration supersedes a failure.
    """
    drop = set(drop)
    unknown = drop - set(reports)
    if unknown:
        raise ConformanceError("cannot drop unknown runs: " + ", ".join(sorted(unknown)))
    kept = {
        run_id: report for run_id, report in reports.items()
        if run_id not in drop
        and report.get("suite", {}).get("version") == matrix["suite"]["version"]
    }
    for report in new_reports:
        validate_report(matrix, report)
        configuration = _configuration(report)
        same = [
            run_id for run_id, existing in kept.items()
            if _configuration(existing) == configuration
        ]
        if any(_finished(kept[run_id]) > _finished(report) for run_id in same):
            continue
        for run_id in same:
            del kept[run_id]
        kept[report_id(report)] = report
    return kept


def publish(matrix: dict, reports: dict[str, dict], *, as_of: date) -> dict:
    """Derive every public status from claims plus retained evidence."""
    freshness = matrix["suite"]["freshnessDays"]
    ordered = [
        (run_id, report) for run_id, report in _ordered(reports)
        if report["suite"]["version"] == matrix["suite"]["version"]
    ]
    cells = []
    for operation in matrix["operations"]:
        for path in matrix["paths"]:
            support = operation["support"][path["id"]]
            covering = []
            for run_id, report in ordered:
                # The socketless file oracle proves ref/object semantics only;
                # it is never relay or failover network evidence.
                if path["id"] != "direct" and report["client"]["transportMode"] != "http":
                    continue
                row = next(
                    row for row in report["operations"] if row["id"] == operation["probe"]
                )
                if path["id"] in row["paths"]:
                    covering.append((run_id, report, row))
            failing = [item for item in covering if item[2]["status"] != "pass"]
            passing = [item for item in covering if item[2]["status"] == "pass"]
            claim = support["status"]
            shown = None
            if claim not in CLAIM_STATUSES:
                status = claim
            elif failing:
                status, shown = "failing", failing[0]
            elif not passing:
                status = "unverified"
            else:
                shown = passing[0]
                age = (as_of - _finished(shown[1]).date()).days
                status = "stale" if age > freshness else claim
            last_run = None
            if shown is not None:
                run_id, report, row = shown
                client = report["client"]
                last_run = {
                    "run": run_id,
                    "date": _finished(report).date().isoformat(),
                    "result": row["status"],
                    "detail": row["detail"],
                    "client": client["label"],
                    "gitVersion": client["gitVersion"],
                    "platform": client["platform"],
                    "protocolVersion": client["protocolVersion"],
                    "transportMode": client["transportMode"],
                    "evidence": f"evidence/{run_id}.json",
                }
            cells.append({
                "operation": operation["id"],
                "operationLabel": operation["label"],
                "path": path["id"],
                "pathLabel": path["label"],
                "transport": path["transport"],
                "authentication": path["authentication"],
                "claim": claim,
                "status": status,
                "expected": support["note"],
                "limitation": support.get("limitation", ""),
                "reproduction": operation["reproduction"],
                "runs": [item[0] for item in covering],
                "lastRun": last_run,
            })
    clients = []
    for client in matrix["clients"]:
        state = client["status"]
        if state == "tested" and (
            as_of - _iso_date(client["lastTested"], client["id"])
        ).days > freshness:
            state = "stale"
        clients.append({**client, "publishedStatus": state})
    claimed = [cell for cell in cells if cell["claim"] in CLAIM_STATUSES]
    verified = sum(cell["status"] == cell["claim"] for cell in claimed)
    return {
        "schema": PUBLISHED_SCHEMA,
        "suite": {
            "id": matrix["suite"]["id"],
            "version": matrix["suite"]["version"],
            "freshnessDays": freshness,
        },
        "summary": {
            "asOf": as_of.isoformat(),
            "freshnessDays": freshness,
            "claims": len(claimed),
            "verified": verified,
            "failing": sum(cell["status"] == "failing" for cell in cells),
            "stale": sum(cell["status"] == "stale" for cell in cells),
            "unverified": sum(cell["status"] == "unverified" for cell in cells),
            "unsupported": sum(cell["claim"] == "unsupported" for cell in cells),
            "passRate": round(verified / len(claimed), 6) if claimed else 0,
            "staleClients": [
                client["id"] for client in clients if client["publishedStatus"] == "stale"
            ],
        },
        "cells": cells,
        "clients": clients,
        "runs": [_run_entry(run_id, report) for run_id, report in ordered],
    }


def degraded_claims(published: dict, *, include_clients: bool = True) -> list[str]:
    problems = []
    for cell in published["cells"]:
        if cell["claim"] not in CLAIM_STATUSES or cell["status"] == cell["claim"]:
            continue
        problem = f"{cell['operation']}/{cell['path']}: claimed {cell['claim']}, published {cell['status']}"
        last = cell["lastRun"]
        if last:
            problem += f" (last run {last['date']}, {last['client']}: {last['detail']})"
        problems.append(problem)
    if include_clients:
        freshness = published["summary"]["freshnessDays"]
        problems.extend(
            f"client {client['id']}: last tested {client['lastTested']}, "
            f"older than {freshness} days"
            for client in published["clients"]
            if client["publishedStatus"] == "stale"
        )
    return problems


def _shell(command: Iterable[str]) -> str:
    return shlex.join(list(command))


def _md(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def _git_version(value: str) -> str:
    return re.sub(r"^git version\s+", "", value)


def _run_reproduction(run: dict) -> str:
    return (
        _shell(run["reproduction"]["checkout"]) + " && "
        + _shell(run["reproduction"]["command"])
    )


def render_markdown(matrix: dict, published: dict) -> str:
    suite = matrix["suite"]
    summary = published["summary"]
    paths = matrix["paths"]
    cells = {(cell["operation"], cell["path"]): cell for cell in published["cells"]}
    lines = [
        "<!-- Generated by tools/git_conformance.py from conformance/git/v1/matrix.json and results.json. Do not edit. -->",
        "# Stock Git compatibility",
        "",
        f"Conformance suite **{suite['id']} {suite['version']}**. Statuses were computed "
        f"**{summary['asOf']}** from {len(published['runs'])} retained stock-Git runs: "
        f"**{summary['verified']} of {summary['claims']}** support claims have a passing "
        f"run from the last {summary['freshnessDays']} days. Supported rows use an "
        f"unmodified Git executable (minimum {suite['minimumGitVersion']}); every run "
        "records the exact client and platform version in its JSON evidence.",
        "",
        "A claim is never published on its own: with no passing run it is "
        "**unverified**, when its newest passing run ages past the freshness window "
        "it is **stale**, and when a retained run failed it is **failing**. "
        "[Filter results by client and workflow](/docs/git-compatibility/) or download "
        "[results.json](/docs/git-compatibility/results.json).",
        "",
        "The offline reference probe uses disposable repositories and the same "
        "smart-HTTP framing as the public read path. Product gateway contract tests "
        "cover authorization, hidden refs, read-only failures, and rollback behavior.",
        "",
        f"Repository shape: {suite['fixture']}",
        "",
        "## Statuses",
        "",
        "| Status | Meaning |",
        "| --- | --- |",
    ]
    for status, meaning in (*matrix["statuses"].items(), *DEGRADED_STATUSES.items()):
        lines.append(f"| **{status}** | {_md(meaning)} |")
    lines.extend([
        "",
        "## Paths",
        "",
        "| Path | Read | Write | Transport | Authentication | Contract |",
        "| --- | --- | --- | --- | --- | --- |",
    ])
    for path in paths:
        lines.append(
            f"| {_md(path['label'])} | {'yes' if path['read'] else 'no'} | "
            f"{'yes' if path['write'] else 'no'} | {_md(path['transport'])} | "
            f"{_md(path['authentication'])} | {_md(path['description'])} |"
        )
    lines.extend([
        "",
        "## Operations",
        "",
        "| Operation | Direct owner SSH | Stable relay HTTPS | Mirror failover | Reproduce |",
        "| --- | --- | --- | --- | --- |",
    ])
    by_path = [path["id"] for path in paths]
    for operation in matrix["operations"]:
        rendered = []
        for path_id in by_path:
            cell = cells[(operation["id"], path_id)]
            text = f"**{cell['status']}** — {_md(cell['expected'])}"
            if cell["status"] != cell["claim"]:
                text += f" Claimed {cell['claim']}."
            if cell["lastRun"]:
                text += f" Last run {cell['lastRun']['date']}."
            if cell["limitation"]:
                text += f" ([limit](#{cell['limitation']}))"
            rendered.append(text)
        lines.append(
            f"| {_md(operation['label'])} | {rendered[0]} | {rendered[1]} | "
            f"{rendered[2]} | `{_md(_shell(operation['reproduction']))}` |"
        )
    lines.extend([
        "",
        "## Evidence runs",
        "",
    ])
    if published["runs"]:
        lines.extend([
            "| Client | Git | Platform | Protocol | Probe transport | Result | Finished | Evidence | Reproduce |",
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- |",
        ])
        for run in published["runs"]:
            client = run["client"]
            result = run["summary"]
            lines.append(
                f"| {_md(client['label'])} | {_md(_git_version(client['gitVersion']))} | "
                f"{_md(client['platform'])} {_md(client['platformRelease'])} "
                f"{_md(client['machine'])} | v{client['protocolVersion']} | "
                f"{client['transportMode']} | **{result['status']}** "
                f"{result['pass']}/{result['pass'] + result['fail'] + result['notRun']} | "
                f"{run['date']} | [report](/docs/git-compatibility/{run['evidence']}) | "
                f"`{_md(_run_reproduction(run))}` |"
            )
    else:
        lines.append("No runs are retained, so every support claim is unverified.")
    lines.extend([
        "",
        "## Tested clients",
        "",
        "| Client | Platform | Version | State | Last tested | Evidence |",
        "| --- | --- | --- | --- | --- | --- |",
    ])
    for client in published["clients"]:
        state = client["publishedStatus"]
        if state != client["status"]:
            state += f" (recorded {client['status']})"
        lines.append(
            f"| {_md(client['name'])} | {_md(client['platform'])} | "
            f"{_md(client['version'])} | **{_md(state)}** | "
            f"{_md(client['lastTested'])} | `{_md(client['evidence'])}` |"
        )
    lines.extend([
        "",
        "## Known limitations",
        "",
    ])
    for limitation in matrix["limitations"]:
        tested = limitation["lastTested"]
        lines.extend([
            f"### {limitation['title']}",
            f"<a id=\"{limitation['id']}\"></a>",
            "",
            f"- Applies to: {', '.join(limitation['appliesTo'])}.",
            f"- Expected result: {limitation['expected']}",
            f"- Reproduce: `{_shell(limitation['reproduction'])}`",
            f"- Action: {limitation['action']}",
            f"- Recovery: {limitation['recovery']}",
            f"- Last tested: {tested['date']} · {tested['client']} · {tested['evidence']}",
            "",
        ])
    lines.extend([
        "## Run the suite",
        "",
        "```sh",
        "python3 tools/git_conformance.py validate",
        "python3 tools/git_conformance.py render --check",
        "python3 tools/git_conformance.py run --protocol-version 0 \\",
        "  --output artifacts/git-conformance-v0.json",
        "python3 tools/git_conformance.py run --protocol-version 2 \\",
        "  --output artifacts/git-conformance-v2.json",
        "python3 tools/git_conformance.py attest \\",
        "  artifacts/git-conformance-v0.json artifacts/git-conformance-v2.json",
        "python3 tools/git_conformance.py gate",
        "```",
        "",
        "Use `--git /path/to/git --client-label 'client version'` to exercise a "
        "desktop application's bundled, unmodified Git. Reports contain operation "
        "status, elapsed milliseconds, exact client/platform versions, generalized "
        "request counts, and test object IDs. They exclude temporary paths, URLs, "
        "repository contents, credentials, and client addresses.",
        "",
    ])
    return "\n".join(lines)


def _h(value: object) -> str:
    return html.escape(str(value), quote=True)


# Kept out of the f-string template so its braces need no escaping. The page is
# static, so freshness is re-checked against the reader's clock: a claim whose
# last passing run ages out degrades before anyone regenerates the page.
_FILTER_SCRIPT = """
      (function () {
        var freshnessDays = __FRESHNESS_DAYS__;
        var claims = { supported: true, local: true, conditional: true };
        var now = Date.now();
        Array.prototype.forEach.call(document.querySelectorAll("[data-last-run]"), function (element) {
          var last = element.getAttribute("data-last-run");
          if (!last || !claims[element.getAttribute("data-status")]) return;
          if (Math.floor((now - Date.parse(last + "T00:00:00Z")) / 86400000) <= freshnessDays) return;
          element.setAttribute("data-status", "stale");
          var badge = element.querySelector(".status");
          if (badge) {
            badge.className = "status status-stale";
            badge.textContent = "stale";
          }
        });
        var form = document.getElementById("gc-filters");
        var count = document.getElementById("gc-count");
        if (!form || !count) return;
        var fields = ["q", "operation", "path", "status", "client"];
        var rows = Array.prototype.slice.call(document.querySelectorAll("#gc-cells tbody tr"));
        var runs = Array.prototype.slice.call(document.querySelectorAll("#gc-runs tbody tr"));
        var params = new URLSearchParams(window.location.search);
        fields.forEach(function (name) {
          if (params.has(name)) form.elements[name].value = params.get(name);
        });
        function apply() {
          var value = {};
          fields.forEach(function (name) { value[name] = form.elements[name].value; });
          var query = value.q.trim().toLowerCase();
          var shown = 0;
          rows.forEach(function (row) {
            var visible = (!value.operation || row.getAttribute("data-operation") === value.operation)
              && (!value.path || row.getAttribute("data-path") === value.path)
              && (!value.status || row.getAttribute("data-status") === value.status)
              && (!value.client || (" " + row.getAttribute("data-runs") + " ").indexOf(" " + value.client + " ") >= 0)
              && (!query || row.getAttribute("data-search").indexOf(query) >= 0);
            row.hidden = !visible;
            if (visible) shown += 1;
          });
          runs.forEach(function (row) {
            row.hidden = Boolean(value.client) && row.getAttribute("data-run") !== value.client;
          });
          count.textContent = shown + " of " + rows.length + " results";
          var next = new URLSearchParams();
          fields.forEach(function (name) { if (value[name]) next.set(name, value[name]); });
          var search = next.toString();
          window.history.replaceState(null, "", window.location.pathname + (search ? "?" + search : "") + window.location.hash);
        }
        form.addEventListener("input", apply);
        form.addEventListener("change", apply);
        apply();
      })();
"""


def _options(pairs: Iterable[tuple[str, str]], everything: str) -> str:
    return '<option value="">%s</option>%s' % (
        _h(everything),
        "".join('<option value="%s">%s</option>' % (_h(value), _h(label)) for value, label in pairs),
    )


def _run_label(run: dict) -> str:
    client = run["client"]
    return "%s · Git %s · %s · protocol v%s · %s" % (
        client["label"], _git_version(client["gitVersion"]), client["platform"],
        client["protocolVersion"], client["transportMode"],
    )


def render_html(matrix: dict, published: dict) -> str:
    suite = matrix["suite"]
    summary = published["summary"]
    cells = {(cell["operation"], cell["path"]): cell for cell in published["cells"]}
    path_rows = "".join(
        "<tr><td><strong>%s</strong></td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>"
        % (
            _h(path["label"]),
            "yes" if path["read"] else "no",
            "yes" if path["write"] else "no",
            _h(path["transport"]),
            _h(path["authentication"]),
            _h(path["description"]),
        )
        for path in matrix["paths"]
    )
    status_rows = "".join(
        '<tr><td><span class="status status-%s">%s</span></td><td>%s</td></tr>'
        % (_h(status), _h(status), _h(meaning))
        for status, meaning in (*matrix["statuses"].items(), *DEGRADED_STATUSES.items())
    )
    operation_rows = []
    for operation in matrix["operations"]:
        rendered = []
        for path_id in ("direct", "relay", "mirror-failover"):
            cell = cells[(operation["id"], path_id)]
            link = (
                ' <a href="#%s">limitation</a>' % _h(cell["limitation"])
                if cell["limitation"] else ""
            )
            last = cell["lastRun"]
            rendered.append(
                '<td data-status="%s" data-last-run="%s"><span class="status status-%s">%s</span><br>%s%s%s</td>'
                % (
                    _h(cell["status"]), _h(last["date"] if last else ""),
                    _h(cell["status"]), _h(cell["status"]), _h(cell["expected"]),
                    '<br><small>Last run %s</small>' % _h(last["date"]) if last else "",
                    link,
                )
            )
        operation_rows.append(
            "<tr><td><strong>%s</strong></td>%s%s%s<td><code>%s</code></td></tr>"
            % (
                _h(operation["label"]), rendered[0], rendered[1], rendered[2],
                _h(_shell(operation["reproduction"])),
            )
        )
    result_rows = []
    for cell in published["cells"]:
        last = cell["lastRun"]
        if last:
            evidence = '%s · %s · Git %s · %s · v%s · %s<br><a href="%s">evidence</a>' % (
                _h(last["date"]), _h(last["client"]), _h(_git_version(last["gitVersion"])),
                _h(last["platform"]), _h(last["protocolVersion"]),
                _h(last["transportMode"]), _h(last["evidence"]),
            )
            if last["result"] != "pass":
                evidence += "<br><small>%s: %s</small>" % (_h(last["result"]), _h(last["detail"]))
        else:
            evidence = "No covering run" if cell["claim"] in CLAIM_STATUSES else "Not a support claim"
        limitation = (
            ' <a href="#%s">limitation</a>' % _h(cell["limitation"])
            if cell["limitation"] else ""
        )
        searchable = " ".join([
            cell["operationLabel"], cell["pathLabel"], cell["transport"],
            cell["authentication"], cell["expected"], cell["status"], cell["claim"],
            *(
                [last["client"], last["gitVersion"], last["platform"], last["transportMode"]]
                if last else []
            ),
        ]).lower()
        result_rows.append(
            '<tr data-operation="%s" data-path="%s" data-status="%s" data-runs="%s" '
            'data-last-run="%s" data-search="%s"><td><strong>%s</strong></td><td>%s</td>'
            '<td>%s</td><td>%s</td><td>%s%s</td><td><span class="status status-%s">%s</span>%s</td>'
            '<td>%s</td><td><code>%s</code></td></tr>'
            % (
                _h(cell["operation"]), _h(cell["path"]), _h(cell["status"]),
                _h(" ".join(cell["runs"])), _h(last["date"] if last else ""),
                _h(searchable), _h(cell["operationLabel"]), _h(cell["pathLabel"]),
                _h(cell["transport"]), _h(cell["authentication"]), _h(cell["expected"]),
                limitation, _h(cell["status"]), _h(cell["status"]),
                "<br><small>claimed %s</small>" % _h(cell["claim"])
                if cell["status"] != cell["claim"] else "",
                evidence, _h(_shell(cell["reproduction"])),
            )
        )
    run_rows = "".join(
        '<tr data-run="%s"><td><strong>%s</strong></td><td>%s</td><td>%s %s %s</td><td>v%s</td>'
        '<td>%s</td><td><span class="status status-%s">%s</span> %d/%d</td><td>%s</td>'
        '<td><a href="%s">report</a></td><td><code>%s</code></td></tr>'
        % (
            _h(run["id"]), _h(run["client"]["label"]),
            _h(_git_version(run["client"]["gitVersion"])), _h(run["client"]["platform"]),
            _h(run["client"]["platformRelease"]), _h(run["client"]["machine"]),
            _h(run["client"]["protocolVersion"]), _h(run["client"]["transportMode"]),
            "supported" if run["summary"]["status"] == "pass" else "failing",
            _h(run["summary"]["status"]), run["summary"]["pass"],
            run["summary"]["pass"] + run["summary"]["fail"] + run["summary"]["notRun"],
            _h(run["date"]), _h(run["evidence"]), _h(_run_reproduction(run)),
        )
        for run in published["runs"]
    ) or '<tr><td colspan="9">No runs are retained, so every support claim is unverified.</td></tr>'
    client_rows = "".join(
        "<tr><td><strong>%s</strong></td><td>%s</td><td>%s</td><td>%s</td>"
        "<td>%s</td><td><code>%s</code></td></tr>"
        % (
            _h(client["name"]), _h(client["platform"]), _h(client["version"]),
            _h(client["publishedStatus"]) + (
                " (recorded %s)" % _h(client["status"])
                if client["publishedStatus"] != client["status"] else ""
            ),
            _h(client["lastTested"]), _h(client["evidence"]),
        )
        for client in published["clients"]
    )
    limitations = []
    for limitation in matrix["limitations"]:
        tested = limitation["lastTested"]
        limitations.append(
            '<section class="limitation" id="%s"><h3>%s</h3><dl>'
            '<dt>Applies to</dt><dd>%s</dd><dt>Expected result</dt><dd>%s</dd>'
            '<dt>Reproduce</dt><dd><code>%s</code></dd><dt>Action</dt><dd>%s</dd>'
            '<dt>Recovery</dt><dd>%s</dd><dt>Last tested</dt><dd>%s · %s · %s</dd>'
            '</dl></section>'
            % (
                _h(limitation["id"]), _h(limitation["title"]),
                _h(", ".join(limitation["appliesTo"])), _h(limitation["expected"]),
                _h(_shell(limitation["reproduction"])), _h(limitation["action"]),
                _h(limitation["recovery"]), _h(tested["date"]),
                _h(tested["client"]), _h(tested["evidence"]),
            )
        )
    filters = (
        '<form class="filters" id="gc-filters" role="search" onsubmit="return false">'
        '<label>Search <input type="search" name="q" placeholder="clone, SSH, Windows"></label>'
        '<label>Operation <select name="operation">%s</select></label>'
        '<label>Path <select name="path">%s</select></label>'
        '<label>Status <select name="status">%s</select></label>'
        '<label>Client <select name="client">%s</select></label>'
        '</form><p id="gc-count" aria-live="polite"></p>'
        % (
            _options(((op["id"], op["label"]) for op in matrix["operations"]), "All operations"),
            _options(((path["id"], path["label"]) for path in matrix["paths"]), "All paths"),
            _options(
                ((status, status) for status in (*matrix["statuses"], *DEGRADED_STATUSES)),
                "All statuses",
            ),
            _options(((run["id"], _run_label(run)) for run in published["runs"]), "All clients"),
        )
    )
    script = _FILTER_SCRIPT.replace("__FRESHNESS_DAYS__", str(summary["freshnessDays"]))
    return f'''<!doctype html>
<!-- Generated by tools/git_conformance.py from conformance/git/v1/matrix.json and results.json. Do not edit. -->
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>Stock Git compatibility · ForkMesh Docs</title>
    <meta name="description" content="Versioned ForkMesh compatibility evidence and actionable limitations for stock Git clients." />
    <meta name="application-name" content="ForkMesh" />
    <meta name="apple-mobile-web-app-title" content="ForkMesh" />
    <meta name="theme-color" content="#090909" />
    <meta name="color-scheme" content="light dark" />
    <link rel="icon" href="/favicon/favicon.ico" sizes="any" />
    <link rel="icon" href="/favicon/favicon.svg" type="image/svg+xml" />
    <link rel="icon" type="image/png" sizes="32x32" href="/favicon/favicon-32x32.png" />
    <link rel="icon" type="image/png" sizes="16x16" href="/favicon/favicon-16x16.png" />
    <link rel="apple-touch-icon" sizes="180x180" href="/favicon/apple-touch-icon.png" />
    <link rel="manifest" href="/favicon/site.webmanifest" />
    <link rel="stylesheet" href="/styles.css" />
    <link rel="stylesheet" href="/site-header.css" />
    <link rel="stylesheet" href="/site-footer.css" />
    <script src="/api-client.js?v=${{version}}"></script>
    <script src="/analytics.js"></script>
    <script src="/safe-input.js" defer></script>
    <script src="/static-page.js?v=${{version}}" defer></script>
    <script src="/site-header.js?v=${{version}}" defer></script>
    <script src="/site-footer.js?v=${{version}}" defer></script>
    <script src="/genie-spider.js?v=${{version}}" defer></script>
    <style>
      .doc-wrap {{ max-width: 1180px; margin: 0 auto; padding: 42px 20px 80px; }}
      .doc-wrap h1 {{ font-size: clamp(32px, 5vw, 48px); margin: 0 0 12px; }}
      .intro {{ color: var(--muted); max-width: 78ch; font-size: 17px; line-height: 1.65; }}
      h2 {{ margin: 44px 0 14px; padding-top: 22px; border-top: 1px solid var(--border); }}
      h3 {{ margin: 26px 0 10px; }}
      p, td, dd {{ color: var(--muted); line-height: 1.55; }}
      a {{ color: var(--link); }}
      .table-wrap {{ overflow-x: auto; border: 1px solid var(--border); border-radius: 12px; }}
      table {{ border-collapse: collapse; width: 100%; min-width: 840px; font-size: 14px; }}
      th, td {{ padding: 11px 12px; text-align: left; vertical-align: top; border-bottom: 1px solid var(--border); }}
      th {{ color: var(--foreground); }}
      tr:last-child td {{ border-bottom: 0; }}
      tr[hidden] {{ display: none; }}
      code {{ font-family: "ForkMesh Mono", ui-monospace, monospace; font-size: .88em; overflow-wrap: anywhere; }}
      .status {{ display: inline-block; border: 1px solid var(--border); border-radius: 999px; padding: 1px 8px; color: var(--foreground); font-size: 12px; font-weight: 700; }}
      .status-supported, .status-local {{ border-color: #27864a; }}
      .status-conditional, .status-stale, .status-unverified {{ border-color: #a77316; }}
      .status-unsupported, .status-failing {{ border-color: #a43d3d; }}
      .filters {{ display: flex; flex-wrap: wrap; gap: 10px 16px; margin: 0 0 10px; }}
      .filters label {{ display: grid; gap: 4px; font-size: 13px; font-weight: 700; color: var(--foreground); }}
      .filters input, .filters select {{ min-width: 180px; padding: 7px 9px; border: 1px solid var(--border); border-radius: 8px; background: transparent; color: var(--foreground); font: inherit; font-weight: 400; }}
      .limitation {{ padding: 4px 0 14px; }}
      dl {{ display: grid; grid-template-columns: 130px minmax(0, 1fr); gap: 8px 16px; }}
      dt {{ font-weight: 700; color: var(--foreground); }}
      dd {{ margin: 0; }}
      pre {{ padding: 16px; border: 1px solid var(--border); border-radius: 10px; overflow-x: auto; }}
      @media (max-width: 640px) {{ dl {{ grid-template-columns: 1fr; }} .filters input, .filters select {{ min-width: 0; width: 100%; }} .filters label {{ flex: 1 1 100%; }} }}
    </style>
  </head>
  <body>
    <div data-forkmesh-header="simple"></div>
    <main class="doc-wrap">
      <p><a href="/docs">Docs</a> / Stock Git compatibility</p>
      <h1>Stock Git compatibility</h1>
      <p class="intro">Conformance suite <strong>{_h(suite['id'])} {_h(suite['version'])}</strong>. Statuses were computed {_h(summary['asOf'])} from {len(published['runs'])} retained stock-Git runs: <strong>{summary['verified']} of {summary['claims']}</strong> support claims have a passing run from the last {summary['freshnessDays']} days. Supported rows run with an unmodified Git executable (minimum {_h(suite['minimumGitVersion'])}); every retained JSON report records the exact Git and platform version.</p>
      <p class="intro">A claim without a passing run is published as unverified, one whose newest passing run is older than {summary['freshnessDays']} days as stale, and one with a failed retained run as failing. This page re-checks freshness against your clock. <a href="results.json">Download the published results</a> or <a href="matrix.json">the source matrix</a>.</p>
      <p class="intro">Repository shape: {_h(suite['fixture'])}</p>
      <h2>Results</h2>
      {filters}
      <div class="table-wrap"><table id="gc-cells"><thead><tr><th>Operation</th><th>Path</th><th>Transport</th><th>Authentication</th><th>Expected result</th><th>Status</th><th>Last run</th><th>Reproduce</th></tr></thead><tbody>{''.join(result_rows)}</tbody></table></div>
      <h2>Evidence runs</h2><div class="table-wrap"><table id="gc-runs"><thead><tr><th>Client</th><th>Git</th><th>Platform</th><th>Protocol</th><th>Probe transport</th><th>Result</th><th>Finished</th><th>Evidence</th><th>Reproduce</th></tr></thead><tbody>{run_rows}</tbody></table></div>
      <h2>Statuses</h2><div class="table-wrap"><table><thead><tr><th>Status</th><th>Meaning</th></tr></thead><tbody>{status_rows}</tbody></table></div>
      <h2>Paths</h2><div class="table-wrap"><table><thead><tr><th>Path</th><th>Read</th><th>Write</th><th>Transport</th><th>Authentication</th><th>Contract</th></tr></thead><tbody>{path_rows}</tbody></table></div>
      <h2>Operations</h2><div class="table-wrap"><table><thead><tr><th>Operation</th><th>Direct owner SSH</th><th>Stable relay HTTPS</th><th>Mirror failover</th><th>Reproduce</th></tr></thead><tbody>{''.join(operation_rows)}</tbody></table></div>
      <h2>Tested clients</h2><div class="table-wrap"><table><thead><tr><th>Client</th><th>Platform</th><th>Version</th><th>State</th><th>Last tested</th><th>Evidence</th></tr></thead><tbody>{client_rows}</tbody></table></div>
      <h2>Known limitations</h2>{''.join(limitations)}
      <h2>Run the suite</h2>
      <pre><code>python3 tools/git_conformance.py validate
python3 tools/git_conformance.py render --check
python3 tools/git_conformance.py run --protocol-version 0 \\
  --output artifacts/git-conformance-v0.json
python3 tools/git_conformance.py run --protocol-version 2 \\
  --output artifacts/git-conformance-v2.json
python3 tools/git_conformance.py attest \\
  artifacts/git-conformance-v0.json artifacts/git-conformance-v2.json
python3 tools/git_conformance.py gate</code></pre>
      <p>Use <code>--git /path/to/git --client-label "client version"</code> for a desktop application's bundled Git. Reports intentionally omit paths, URLs, repository contents, credentials, and client addresses.</p>
    </main>
    <div data-forkmesh-footer="standard"></div>
    <script>{script}    </script>
  </body>
</html>
'''


def generated_outputs(
    matrix: dict, reports: dict[str, dict], attested: str
) -> dict[Path, str]:
    published = publish(matrix, reports, as_of=date.fromisoformat(attested))
    outputs = {
        MARKDOWN_PATH: render_markdown(matrix, published),
        PUBLIC_HTML_PATH: render_html(matrix, published),
        PUBLIC_JSON_PATH: json.dumps(matrix, indent=2, ensure_ascii=False) + "\n",
        PUBLIC_RESULTS_PATH: json.dumps(published, indent=2, ensure_ascii=False) + "\n",
    }
    for run_id, report in reports.items():
        outputs[PUBLIC_EVIDENCE_DIRECTORY / f"{run_id}.json"] = _evidence_text(report)
    return outputs


def _sync_files(outputs: dict[Path, str], directory: Path, *, check: bool) -> list[str]:
    """Write outputs and remove files in ``directory`` no output names."""
    stale = []
    for path, expected in outputs.items():
        if check:
            try:
                actual = path.read_text(encoding="utf-8")
            except OSError:
                actual = ""
            if actual != expected:
                stale.append(_display(path))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(expected, encoding="utf-8", newline="\n")
    if directory.exists():
        for orphan in sorted(set(directory.iterdir()) - set(outputs)):
            if check:
                stale.append(_display(orphan))
            else:
                orphan.unlink()
    return stale


def render_outputs(
    matrix: dict, reports: dict[str, dict], attested: str, *, check: bool
) -> None:
    stale = _sync_files(
        generated_outputs(matrix, reports, attested),
        PUBLIC_EVIDENCE_DIRECTORY,
        check=check,
    )
    if stale:
        raise ConformanceError(
            "generated compatibility output is stale: " + ", ".join(stale)
            + "; run python3 tools/git_conformance.py render"
        )


def write_evidence(matrix: dict, reports: dict[str, dict], attested: str) -> None:
    outputs = {
        EVIDENCE_DIRECTORY / f"{run_id}.json": _evidence_text(report)
        for run_id, report in reports.items()
    }
    outputs[RESULTS_PATH] = json.dumps(
        results_document(matrix, reports, attested), indent=2, ensure_ascii=False
    ) + "\n"
    EVIDENCE_DIRECTORY.mkdir(parents=True, exist_ok=True)
    _sync_files(outputs, EVIDENCE_DIRECTORY, check=False)
    render_outputs(matrix, reports, attested, check=False)


def _child_environment(extra: dict[str, str] | None = None) -> dict[str, str]:
    environment = dict(os.environ)
    environment.update({
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_ALLOW_PROTOCOL": "file:http:https",
        "LANG": "C",
        "LC_ALL": "C",
    })
    if extra:
        environment.update(extra)
    return environment


class GitRunner:
    def __init__(self, executable: str, protocol_version: str) -> None:
        self.executable = executable
        self.protocol_version = protocol_version

    def command(self, arguments: Iterable[str]) -> list[str]:
        return [
            self.executable,
            "-c",
            f"protocol.version={self.protocol_version}",
            *list(arguments),
        ]

    def run(
        self,
        arguments: Iterable[str],
        *,
        cwd: Path | None = None,
        input_bytes: bytes | None = None,
        check: bool = True,
        extra_environment: dict[str, str] | None = None,
        timeout: int = 90,
    ) -> subprocess.CompletedProcess[bytes]:
        command = self.command(arguments)
        try:
            completed = subprocess.run(
                command,
                cwd=cwd,
                input=input_bytes,
                capture_output=True,
                env=_child_environment(extra_environment),
                timeout=timeout,
                check=False,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise ConformanceError(f"Git command could not run: {_shell(command)}: {exc}") from exc
        if check and completed.returncode:
            detail = completed.stderr.decode("utf-8", "replace").strip()
            raise ConformanceError(
                f"Git command failed ({completed.returncode}): {_shell(command)}: {detail[:1000]}"
            )
        return completed

    def text(self, arguments: Iterable[str], *, cwd: Path | None = None) -> str:
        return self.run(arguments, cwd=cwd).stdout.decode("utf-8", "replace").strip()

    def oid(self, repository: Path, revision: str = "HEAD") -> str:
        value = self.text(["-C", str(repository), "rev-parse", "--verify", revision])
        if not re.fullmatch(r"[0-9a-f]{40,64}", value):
            raise ConformanceError(f"Git returned an invalid object id for {revision}")
        return value

    def version_tuple(self) -> tuple[int, int, int]:
        value = self.text(["--version"])
        match = re.search(r"\b([0-9]+)\.([0-9]+)\.([0-9]+)", value)
        if not match:
            raise ConformanceError(f"cannot parse Git version: {value}")
        return tuple(int(part) for part in match.groups())


class _QuietHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "ForkMeshConformance/1"
    sys_version = ""

    def log_message(self, _format: str, *_arguments: object) -> None:
        return

    def do_GET(self) -> None:  # noqa: N802
        self._dispatch()

    def do_HEAD(self) -> None:  # noqa: N802
        self._dispatch()

    def do_POST(self) -> None:  # noqa: N802
        self._dispatch()

    def _body(self) -> bytes:
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError as exc:
            raise ConformanceError("invalid fixture Content-Length") from exc
        if length < 0 or length > 64 * 1024 * 1024:
            raise ConformanceError("fixture request body is outside the test bound")
        return self.rfile.read(length) if length else b""

    def _dispatch(self) -> None:
        try:
            status, headers, body = self.server.application.handle(  # type: ignore[attr-defined]
                self.command, self.path, dict(self.headers.items()), self._body()
            )
        except Exception:
            status, headers, body = 500, {"Content-Type": "text/plain"}, b"fixture failure\n"
        self.send_response(status)
        sent_length = False
        for name, value in headers.items():
            if name.lower() in {"connection", "transfer-encoding", "server", "date"}:
                continue
            if name.lower() == "content-length":
                sent_length = True
            self.send_header(name, value)
        if not sent_length:
            self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        if self.command != "HEAD" and body:
            self.wfile.write(body)


class _LoopbackHTTPServer(ThreadingHTTPServer):
    """A fixture server bound to whichever loopback family the runner allows."""

    def __init__(self, host: str, handler: object) -> None:
        self.address_family = socket.AF_INET6 if ":" in host else socket.AF_INET
        super().__init__((host, 0), handler)  # type: ignore[arg-type]


def _loopback_url(host: str, port: int) -> str:
    return f"http://[{host}]:{port}" if ":" in host else f"http://{host}:{port}"


class LocalHTTPService:
    def __init__(self, application: object, host: str) -> None:
        self.application = application
        self.host = host
        self.server = _LoopbackHTTPServer(host, _QuietHandler)
        self.server.daemon_threads = True
        self.server.application = application  # type: ignore[attr-defined]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        return _loopback_url(self.host, self.server.server_address[1])

    def start(self) -> None:
        self.thread.start()

    def close(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def __enter__(self) -> "LocalHTTPService":
        self.start()
        return self

    def __exit__(self, *_arguments: object) -> None:
        self.close()


def _parse_cgi_response(raw: bytes) -> tuple[int, dict[str, str], bytes]:
    marker = b"\r\n\r\n"
    if marker not in raw:
        marker = b"\n\n"
    if marker not in raw:
        raise ConformanceError("git http-backend returned malformed CGI headers")
    header_bytes, body = raw.split(marker, 1)
    status = 200
    headers: dict[str, str] = {}
    for line in header_bytes.decode("latin-1").splitlines():
        if ":" not in line:
            raise ConformanceError("git http-backend returned a malformed header")
        name, value = (part.strip() for part in line.split(":", 1))
        if name.lower() == "status":
            try:
                status = int(value.split(" ", 1)[0])
            except ValueError as exc:
                raise ConformanceError("git http-backend returned an invalid status") from exc
        else:
            headers[name] = value
    return status, headers, body


class GitHTTPBackend:
    def __init__(self, root: Path, git: GitRunner, remote_addr: str) -> None:
        self.root = root
        self.git = git
        self.remote_addr = remote_addr
        self.request_count = 0
        self._lock = threading.Lock()

    def handle(
        self, method: str, target: str, headers: dict[str, str], body: bytes
    ) -> tuple[int, dict[str, str], bytes]:
        parsed = urlsplit(target)
        environment = {
            "GIT_PROJECT_ROOT": str(self.root),
            "GIT_HTTP_EXPORT_ALL": "1",
            "PATH_INFO": unquote(parsed.path),
            "QUERY_STRING": parsed.query,
            "REQUEST_METHOD": method,
            "CONTENT_TYPE": headers.get("Content-Type", ""),
            "CONTENT_LENGTH": str(len(body)),
            "REMOTE_ADDR": self.remote_addr,
            "SERVER_PROTOCOL": "HTTP/1.1",
        }
        protocol = headers.get("Git-Protocol", "").strip()
        if protocol in {"version=1", "version=2"}:
            environment["HTTP_GIT_PROTOCOL"] = protocol
        with self._lock:
            self.request_count += 1
        completed = self.git.run(
            ["http-backend"],
            input_bytes=body,
            check=False,
            extra_environment=environment,
            timeout=60,
        )
        if completed.returncode:
            raise ConformanceError("git http-backend failed")
        return _parse_cgi_response(completed.stdout)


class UnavailableOrigin:
    def __init__(self) -> None:
        self.request_count = 0
        self._lock = threading.Lock()

    def handle(
        self, _method: str, _target: str, _headers: dict[str, str], _body: bytes
    ) -> tuple[int, dict[str, str], bytes]:
        with self._lock:
            self.request_count += 1
        return 503, {"Content-Type": "text/plain"}, b"origin unavailable\n"


class RelayProxy:
    def __init__(self, upstreams: list[str]) -> None:
        self.upstreams = upstreams
        self.attempts = 0
        self.successes = 0
        self._lock = threading.Lock()

    def handle(
        self, method: str, target: str, headers: dict[str, str], body: bytes
    ) -> tuple[int, dict[str, str], bytes]:
        last = (503, {"Content-Type": "text/plain"}, b"mirror unavailable\n")
        for upstream in self.upstreams:
            parsed = urlsplit(upstream)
            connection = HTTPConnection(parsed.hostname, parsed.port, timeout=20)
            forwarded = {
                name: value for name, value in headers.items()
                if name.lower() in {"content-type", "content-encoding", "git-protocol", "user-agent"}
            }
            try:
                with self._lock:
                    self.attempts += 1
                connection.request(method, target, body=body, headers=forwarded)
                response = connection.getresponse()
                response_body = response.read()
                response_headers = {
                    name: value for name, value in response.getheaders()
                    if name.lower() not in {"connection", "transfer-encoding", "server", "date"}
                }
                last = (response.status, response_headers, response_body)
                if response.status not in RETRYABLE_PROXY_STATUSES:
                    with self._lock:
                        self.successes += 1
                    return last
            except OSError:
                last = (503, {"Content-Type": "text/plain"}, b"mirror unavailable\n")
            finally:
                connection.close()
        return last


def _configure_identity(git: GitRunner, repository: Path) -> None:
    git.run(["-C", str(repository), "config", "user.name", "ForkMesh Conformance"])
    git.run(["-C", str(repository), "config", "user.email", "conformance@example.invalid"])


def _commit_file(
    git: GitRunner, repository: Path, relative: str, content: bytes, message: str
) -> str:
    target = repository / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    git.run(["-C", str(repository), "add", "--", relative])
    git.run(["-C", str(repository), "commit", "-m", message])
    return git.oid(repository)


class Fixture:
    def __init__(self, root: Path, git: GitRunner) -> None:
        self.root = root
        self.git = git
        self.primary = root / "primary"
        self.mirror = root / "mirror"
        self.seed = root / "seed"
        self.allowed_signers = root / "allowed_signers"
        self.signed_oid = ""
        self.signed_bytes = b""
        self.submodule_oid = ""
        self.initial_main = ""

    @property
    def primary_repo(self) -> Path:
        return self.primary / "project.git"

    @property
    def mirror_repo(self) -> Path:
        return self.mirror / "project.git"

    def create(self) -> None:
        self.primary.mkdir(parents=True)
        sub_work = self.root / "submodule-work"
        self.git.run(["init", "-q", "-b", "main", str(sub_work)])
        _configure_identity(self.git, sub_work)
        self.submodule_oid = _commit_file(
            self.git, sub_work, "submodule.txt", b"submodule object\n", "submodule fixture"
        )
        self.git.run(["clone", "-q", "--bare", str(sub_work), str(self.primary / "submodule.git")])

        self.git.run(["init", "-q", "-b", "main", str(self.seed)])
        _configure_identity(self.git, self.seed)
        (self.seed / "README.md").write_text("# stock Git conformance\n", encoding="utf-8")
        (self.seed / "payload.bin").write_bytes(bytes(range(256)) * 512)
        (self.seed / ".gitmodules").write_text(
            '[submodule "fixture"]\n\tpath = deps/fixture\n\turl = ../submodule.git\n',
            encoding="utf-8",
        )
        self.git.run(["-C", str(self.seed), "add", "README.md", "payload.bin", ".gitmodules"])
        self.git.run([
            "-C", str(self.seed), "update-index", "--add", "--cacheinfo",
            f"160000,{self.submodule_oid},deps/fixture",
        ])
        key = self.root / "signing_key"
        keygen = shutil.which("ssh-keygen")
        if not keygen:
            raise ConformanceError("ssh-keygen is required for the signed-commit probe")
        generated = subprocess.run(
            [keygen, "-q", "-t", "ed25519", "-N", "", "-C", "conformance", "-f", str(key)],
            capture_output=True,
            env=_child_environment(),
            timeout=30,
            check=False,
        )
        if generated.returncode:
            raise ConformanceError("ssh-keygen could not create the disposable signing key")
        public_parts = (key.with_suffix(key.suffix + ".pub")).read_text(
            encoding="ascii"
        ).strip().split()
        if len(public_parts) < 2:
            raise ConformanceError("ssh-keygen returned an invalid public key")
        self.allowed_signers.write_text(
            f"conformance@example.invalid {public_parts[0]} {public_parts[1]}\n",
            encoding="ascii",
        )
        self.git.run(["-C", str(self.seed), "config", "gpg.format", "ssh"])
        self.git.run(["-C", str(self.seed), "config", "user.signingkey", str(key)])
        fixed_time = {
            "GIT_AUTHOR_DATE": "2001-01-01T00:00:00+00:00",
            "GIT_COMMITTER_DATE": "2001-01-01T00:00:00+00:00",
        }
        self.git.run(
            ["-C", str(self.seed), "commit", "-S", "-m", "signed fixture"],
            extra_environment=fixed_time,
        )
        self.signed_oid = self.git.oid(self.seed)
        self.signed_bytes = self.git.run(
            ["-C", str(self.seed), "cat-file", "commit", self.signed_oid]
        ).stdout
        if b"gpgsig -----BEGIN SSH SIGNATURE-----" not in self.signed_bytes:
            raise ConformanceError("fixture commit does not contain an SSH signature")
        self.git.run(["-C", str(self.seed), "config", "commit.gpgSign", "false"])
        for index in (2, 3):
            _commit_file(
                self.git,
                self.seed,
                "history.txt",
                ("history %d\n" % index).encode("ascii"),
                "history %d" % index,
            )
        self.initial_main = self.git.oid(self.seed)
        self.git.run(["-C", str(self.seed), "tag", "conformance-v1", self.signed_oid])
        self.git.run(["clone", "-q", "--bare", str(self.seed), str(self.primary_repo)])
        self._configure_bare(self.primary_repo, writable=True)
        self.git.run(["clone", "-q", "--bare", str(self.primary_repo), str(self.primary / "read-only.git")])
        self._configure_bare(self.primary / "read-only.git", writable=False)
        shutil.copytree(self.primary, self.mirror)

    def _configure_bare(self, repository: Path, *, writable: bool) -> None:
        for key, value in (
            ("http.receivepack", "true" if writable else "false"),
            ("uploadpack.allowFilter", "true"),
            ("receive.fsckObjects", "true"),
            ("transfer.fsckObjects", "true"),
            ("core.logAllRefUpdates", "true"),
        ):
            self.git.run(["--git-dir", str(repository), "config", key, value])
        if not writable:
            hook = repository / "hooks" / "pre-receive"
            hook.write_text(
                "#!/bin/sh\necho conformance-read-only >&2\nexit 1\n",
                encoding="utf-8",
            )
            hook.chmod(0o700)

    def sync_mirror(self) -> None:
        for name in ("project.git", "submodule.git"):
            destination = self.mirror / name
            source = self.primary / name
            self.git.run([
                "--git-dir", str(destination), "fetch", "--prune", str(source),
                "+refs/heads/*:refs/heads/*", "+refs/tags/*:refs/tags/*",
            ])


class Recorder:
    def __init__(self, matrix: dict, fixture_root: Path) -> None:
        self.fixture_root = str(fixture_root)
        path_ids = [path["id"] for path in matrix["paths"]]
        self.rows = {
            operation["probe"]: {
                "id": operation["probe"],
                "status": "not-run",
                "paths": [
                    path_id for path_id in path_ids
                    if operation["support"][path_id]["status"] != "unsupported"
                ],
                "durationMs": 0,
                "detail": "dependency did not complete",
            }
            for operation in matrix["operations"]
        }

    def _safe(self, value: object) -> str:
        text = str(value).replace(self.fixture_root, "<fixture>")
        text = re.sub(r"http://(?:127\.0\.0\.1|\[::1\]):[0-9]+", "<fixture-url>", text)
        return text[:800]

    @contextmanager
    def probe(self, identifier: str, paths: Iterable[str]) -> Iterator[None]:
        if identifier not in self.rows:
            raise ConformanceError(f"unknown conformance probe: {identifier}")
        row = self.rows[identifier]
        requested_paths = list(paths)
        if requested_paths != row["paths"]:
            raise ConformanceError(
                f"{identifier}: probe paths do not match the versioned support matrix"
            )
        row.update({"status": "running", "detail": ""})
        started = time.monotonic()
        try:
            yield
        except BaseException as exc:
            row["status"] = "fail"
            row["detail"] = self._safe(exc)
            raise
        else:
            row["status"] = "pass"
            row["detail"] = "stock Git behavior and integrity assertions passed"
        finally:
            row["durationMs"] = max(0, round((time.monotonic() - started) * 1000))


def _assert(condition: bool, message: str) -> None:
    if not condition:
        raise ConformanceError(message)


def _refs(git: GitRunner, repository: Path) -> bytes:
    return git.run([
        "--git-dir", str(repository), "for-each-ref", "--sort=refname",
        "--format=%(objectname) %(refname)", "refs/heads", "refs/tags",
    ]).stdout


class ConformanceSuite:
    def __init__(
        self,
        matrix: dict,
        git: GitRunner,
        root: Path,
        client_label: str,
        transport_mode: str,
    ) -> None:
        self.matrix = matrix
        self.git = git
        self.root = root
        self.client_label = client_label
        self.transport_mode = transport_mode
        self.actual_transport_mode = ""
        self.fixture = Fixture(root, git)
        self.recorder = Recorder(matrix, root)
        self.clone_paths: dict[str, Path] = {}
        self.urls: dict[str, str] = {}
        self.bundle_path = root / "recovery.bundle"
        self.lease_before_force = ""
        self.telemetry: dict[str, object] = {}

    def _clone(self, url: str, destination: Path, *options: str) -> None:
        self.git.run(["clone", "-q", *options, url, str(destination)])

    def _configure_clone(self, repository: Path) -> None:
        _configure_identity(self.git, repository)
        self.git.run(["-C", str(repository), "config", "commit.gpgSign", "false"])

    def _verify_signature(self, repository: Path) -> None:
        raw = self.git.run([
            "-C", str(repository), "cat-file", "commit", self.fixture.signed_oid
        ]).stdout
        _assert(raw == self.fixture.signed_bytes, "signed commit bytes changed in transport")
        verified = self.git.run([
            "-C", str(repository), "-c",
            f"gpg.ssh.allowedSignersFile={self.fixture.allowed_signers}",
            "verify-commit", self.fixture.signed_oid,
        ], check=False)
        _assert(verified.returncode == 0, "transported SSH-signed commit does not verify")

    def _healthy(self, repository: Path) -> None:
        result = self.git.run(
            ["--git-dir", str(repository), "fsck", "--full", "--strict"], check=False
        )
        _assert(result.returncode == 0, "healthy repository failed git fsck")

    def execute(self) -> dict:
        started_at = datetime.now(timezone.utc)
        failure = ""
        direct_backend = mirror_backend = unavailable = relay_proxy = failover_proxy = None
        try:
            minimum = tuple(
                int(part)
                for part in self.matrix["suite"]["minimumGitVersion"].split(".")
            )
            if self.git.version_tuple() < minimum:
                raise ConformanceError(
                    "Git %s or newer is required by conformance suite %s"
                    % (
                        self.matrix["suite"]["minimumGitVersion"],
                        self.matrix["suite"]["version"],
                    )
                )
            self.fixture.create()
            loopback_host = (
                self._loopback_host()
                if self.transport_mode in {"auto", "http"}
                else None
            )
            if self.transport_mode == "http" and loopback_host is None:
                raise ConformanceError(
                    "HTTP transport was required but no loopback address on this "
                    "runner completes a round trip"
                )
            if loopback_host is not None:
                self.actual_transport_mode = "http"
                direct_backend = GitHTTPBackend(
                    self.fixture.primary, self.git, loopback_host
                )
                mirror_backend = GitHTTPBackend(
                    self.fixture.mirror, self.git, loopback_host
                )
                unavailable = UnavailableOrigin()
                with (
                    LocalHTTPService(direct_backend, loopback_host) as direct,
                    LocalHTTPService(mirror_backend, loopback_host) as mirror,
                    LocalHTTPService(unavailable, loopback_host) as dead,
                ):
                    relay_proxy = RelayProxy([direct.base_url])
                    failover_proxy = RelayProxy([dead.base_url, mirror.base_url])
                    with (
                        LocalHTTPService(relay_proxy, loopback_host) as relay,
                        LocalHTTPService(failover_proxy, loopback_host) as failover,
                    ):
                        self.urls = {
                            "direct": direct.base_url + "/project.git",
                            "relay": relay.base_url + "/project.git",
                            "mirror-failover": failover.base_url + "/project.git",
                        }
                        self._execute_operations(direct.base_url)
            else:
                self.actual_transport_mode = "file"
                # No loopback family completes a round trip on this runner.
                # File URLs still invoke stock upload-pack/receive-pack and
                # exercise all ref/object semantics. They are explicitly
                # labeled as an oracle, not as relay-network evidence.
                self.urls = {
                    "direct": self.fixture.primary_repo.as_uri(),
                    "relay": self.fixture.primary_repo.as_uri(),
                    "mirror-failover": self.fixture.mirror_repo.as_uri(),
                }
                self.telemetry["socketlessFailoverSelectionChecks"] = 1
                self._execute_operations(self.fixture.primary.as_uri())
        except BaseException as exc:
            failure = self.recorder._safe(exc)

        finished_at = datetime.now(timezone.utc)
        rows = [
            self.recorder.rows[operation["probe"]]
            for operation in self.matrix["operations"]
        ]
        pass_count = sum(row["status"] == "pass" for row in rows)
        fail_count = sum(row["status"] == "fail" for row in rows)
        compatibility_total = sum(len(row["paths"]) for row in rows)
        compatibility_passed = sum(
            len(row["paths"]) for row in rows if row["status"] == "pass"
        )
        self.telemetry.update({
            "directOriginRequests": getattr(direct_backend, "request_count", 0),
            "mirrorOriginRequests": getattr(mirror_backend, "request_count", 0),
            "unavailableOriginAttempts": getattr(unavailable, "request_count", 0),
            "relayProxyAttempts": getattr(relay_proxy, "attempts", 0),
            "relayProxySuccesses": getattr(relay_proxy, "successes", 0),
            "failoverProxyAttempts": getattr(failover_proxy, "attempts", 0),
            "failoverProxySuccesses": getattr(failover_proxy, "successes", 0),
        })
        version = self.git.text(["--version"])
        tested_commit = self.git.text(["-C", str(ROOT), "rev-parse", "HEAD"])
        return {
            "schema": REPORT_SCHEMA,
            "suite": {
                "id": self.matrix["suite"]["id"],
                "version": self.matrix["suite"]["version"],
            },
            "testedCommit": tested_commit,
            "startedAt": started_at.isoformat(),
            "finishedAt": finished_at.isoformat(),
            "client": {
                "label": self.client_label,
                "gitVersion": version,
                "protocolVersion": self.git.protocol_version,
                "transportMode": self.actual_transport_mode or self.transport_mode,
                "platform": platform.system(),
                "platformRelease": platform.release(),
                "machine": platform.machine(),
                "pythonVersion": platform.python_version(),
            },
            "summary": {
                "status": "pass" if pass_count == len(rows) else "fail",
                "pass": pass_count,
                "fail": fail_count,
                "notRun": len(rows) - pass_count - fail_count,
                "failure": failure,
                "compatibility": {
                    "passedOperationPathCells": compatibility_passed,
                    "totalOperationPathCells": compatibility_total,
                    "passRate": (
                        round(compatibility_passed / compatibility_total, 6)
                        if compatibility_total else 0
                    ),
                },
            },
            "operations": rows,
            "paths": {
                "direct": {"fixture": (
                    "stock smart HTTP write/read origin"
                    if self.actual_transport_mode == "http"
                    else "socketless stock-Git file transport oracle"
                )},
                "relay": {"fixture": (
                    "in-place single-origin HTTP proxy"
                    if self.actual_transport_mode == "http"
                    else "socketless object/ref oracle; no relay-network claim"
                )},
                "mirror-failover": {
                    "fixture": (
                        "retryable failed origin followed by matching mirror"
                        if self.actual_transport_mode == "http"
                        else "socketless matching-mirror oracle; no retry-network claim"
                    ),
                },
            },
            "telemetry": self.telemetry,
            "integrity": {
                "signedCommit": self.fixture.signed_oid or "",
                "mainAtFixtureCreation": self.fixture.initial_main or "",
                "refsSha256": (
                    hashlib.sha256(_refs(self.git, self.fixture.primary_repo)).hexdigest()
                    if self.fixture.primary_repo.is_dir() else ""
                ),
                "temporaryPathsIncluded": False,
                "repositoryContentsIncluded": False,
                "credentialsIncluded": False,
                "clientAddressesIncluded": False,
            },
        }

    @staticmethod
    def _loopback_host() -> str | None:
        """Return the first loopback address that survives a full round trip.

        A successful bind proves nothing.  A runner can accept a bind on
        127.0.0.1 and then drop the traffic, in which case every Git command
        would hang until its timeout instead of degrading to the socketless
        oracle.  IPv4 is probed first so ordinary runners keep their existing
        fixture URLs, and IPv6 is the fallback before giving up on sockets.
        """
        for host in ("127.0.0.1", "::1"):
            try:
                server = _LoopbackHTTPServer(host, _QuietHandler)
            except OSError:
                continue
            server.daemon_threads = True
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with socket.create_connection(
                    (host, server.server_address[1]), timeout=2
                ):
                    return host
            except OSError:
                continue
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)
        return None

    def _execute_operations(self, direct_base: str) -> None:
        all_paths = ["direct", "relay", "mirror-failover"]
        with self.recorder.probe("clone", all_paths):
            for path_id, url in self.urls.items():
                destination = self.root / f"clone-{path_id}"
                self._clone(url, destination)
                self._configure_clone(destination)
                _assert(
                    self.git.oid(destination) == self.fixture.initial_main,
                    f"{path_id} clone changed HEAD object id",
                )
                self.git.run(["-C", str(destination), "fsck", "--full", "--strict"])
                self.clone_paths[path_id] = destination

        with self.recorder.probe("signed-commits", all_paths):
            for repository in self.clone_paths.values():
                self._verify_signature(repository)

        with self.recorder.probe("fetch", all_paths):
            self.git.run(["-C", str(self.fixture.seed), "switch", "-q", "-c", "fetch-fixture"])
            fetched_oid = _commit_file(
                self.git, self.fixture.seed, "fetch.txt", b"fetch update\n", "fetch fixture"
            )
            self.git.run([
                "-C", str(self.fixture.seed), "tag", "fetch-conformance-v1", fetched_oid
            ])
            self.git.run([
                "-C", str(self.fixture.seed), "push", "-q", self.urls["direct"],
                "HEAD:refs/heads/fetch-fixture", "refs/tags/fetch-conformance-v1",
            ])
            self.fixture.sync_mirror()
            for repository in self.clone_paths.values():
                self.git.run(["-C", str(repository), "fetch", "-q", "--prune", "origin"])
                _assert(
                    self.git.oid(repository, "refs/remotes/origin/fetch-fixture") == fetched_oid,
                    "fetch did not preserve the published branch object id",
                )
                _assert(
                    self.git.oid(repository, "refs/tags/fetch-conformance-v1") == fetched_oid,
                    "fetch did not preserve the published tag object id",
                )

        direct_clone = self.clone_paths["direct"]
        with self.recorder.probe("hooks", all_paths):
            self.git.run([
                "-C", str(direct_clone), "switch", "-q", "-c", "hooks-local", "origin/main"
            ])
            (direct_clone / "hook.txt").write_text("hook fixture\n", encoding="utf-8")
            self.git.run(["-C", str(direct_clone), "add", "hook.txt"])
            hook = direct_clone / ".git" / "hooks" / "pre-commit"
            hook.write_text("#!/bin/sh\necho forkmesh-hook-rejected >&2\nexit 37\n", encoding="utf-8")
            hook.chmod(0o700)
            rejected = self.git.run(
                ["-C", str(direct_clone), "commit", "-m", "hook must reject"], check=False
            )
            _assert(rejected.returncode != 0, "client pre-commit hook did not reject commit")
            _assert(
                b"forkmesh-hook-rejected" in rejected.stderr,
                "client hook failure was not actionable",
            )
            hook.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            hook.chmod(0o700)
            self.git.run(["-C", str(direct_clone), "commit", "-m", "hook accepted"])

        with self.recorder.probe("push", ["direct"]):
            self.git.run([
                "-C", str(direct_clone), "switch", "-q", "-c", "push-accepted", "origin/main"
            ])
            pushed_oid = _commit_file(
                self.git, direct_clone, "push.txt", b"accepted push\n", "accepted push"
            )
            self.git.run([
                "-C", str(direct_clone), "push", "-q", "origin",
                "HEAD:refs/heads/push-accepted",
            ])
            _assert(
                self.git.oid(self.fixture.primary_repo, "refs/heads/push-accepted") == pushed_oid,
                "accepted push published the wrong object id",
            )
            read_only_before = self.git.oid(self.fixture.primary / "read-only.git", "refs/heads/main")
            rejected = self.git.run([
                "-C", str(direct_clone), "push", direct_base + "/read-only.git",
                "HEAD:refs/heads/main",
            ], check=False)
            _assert(rejected.returncode != 0, "read-only origin accepted a push")
            _assert(
                self.git.oid(self.fixture.primary / "read-only.git", "refs/heads/main")
                == read_only_before,
                "rejected push changed the read-only ref",
            )
            self._healthy(self.fixture.primary_repo)
            self.fixture.sync_mirror()

        with self.recorder.probe("branches", all_paths):
            for path_id, url in self.urls.items():
                advertised = self.git.text(["ls-remote", "--heads", url, "push-accepted"])
                _assert(pushed_oid in advertised, f"{path_id} did not advertise pushed branch")
            self.git.run([
                "-C", str(direct_clone), "push", "-q", "--delete", "origin", "push-accepted"
            ])
            _assert(
                self.git.run([
                    "--git-dir", str(self.fixture.primary_repo), "rev-parse", "--verify",
                    "refs/heads/push-accepted",
                ], check=False).returncode != 0,
                "deleted branch remains on the writable origin",
            )
            self.fixture.sync_mirror()
            for path_id, repository in self.clone_paths.items():
                self.git.run(["-C", str(repository), "fetch", "-q", "--prune", "origin"])
                _assert(
                    self.git.run([
                        "-C", str(repository), "rev-parse", "--verify",
                        "refs/remotes/origin/push-accepted",
                    ], check=False).returncode != 0,
                    f"{path_id} retained the deleted remote branch",
                )

        with self.recorder.probe("rebase", all_paths):
            local = self.root / "rebase-local"
            writer = self.root / "rebase-writer"
            self._clone(self.urls["relay"], local)
            self._clone(self.urls["direct"], writer)
            self._configure_clone(local)
            self._configure_clone(writer)
            self.git.run(["-C", str(local), "switch", "-q", "-c", "rebased-topic"])
            _commit_file(self.git, local, "local.txt", b"local rebase commit\n", "local change")
            main_oid = _commit_file(
                self.git, writer, "upstream.txt", b"upstream main\n", "upstream change"
            )
            self.git.run(["-C", str(writer), "push", "-q", "origin", "HEAD:main"])
            self.git.run(["-C", str(local), "fetch", "-q", "origin"])
            self.git.run(["-C", str(local), "rebase", "origin/main"])
            parent = self.git.oid(local, "HEAD^")
            _assert(parent == main_oid, "rebased commit is not based on fetched main")
            self.fixture.sync_mirror()

        lease_owner = self.root / "lease-owner"
        lease_contender = self.root / "lease-contender"
        with self.recorder.probe("force-with-lease", ["direct"]):
            self._clone(self.urls["direct"], lease_owner)
            self._clone(self.urls["direct"], lease_contender)
            self._configure_clone(lease_owner)
            self._configure_clone(lease_contender)
            for repository in (lease_owner, lease_contender):
                self.git.run([
                    "-C", str(repository), "switch", "-q", "-c", "lease", "origin/main"
                ])
            self.git.run([
                "-C", str(lease_owner), "push", "-q", "-u", "origin", "lease"
            ])
            self.git.run(["-C", str(lease_contender), "fetch", "-q", "origin", "lease"])
            self.git.run([
                "-C", str(lease_contender), "branch", "--set-upstream-to=origin/lease", "lease"
            ])
            owner_oid = _commit_file(
                self.git, lease_owner, "owner.txt", b"owner lease\n", "owner lease"
            )
            contender_oid = _commit_file(
                self.git, lease_contender, "contender.txt", b"contender lease\n", "contender lease"
            )
            self.git.run([
                "-C", str(lease_contender), "push", "-q", "origin", "lease"
            ])
            stale = self.git.run([
                "-C", str(lease_owner), "push", "--force-with-lease", "origin", "lease"
            ], check=False)
            _assert(stale.returncode != 0, "stale force-with-lease was accepted")
            _assert(
                self.git.oid(self.fixture.primary_repo, "refs/heads/lease") == contender_oid,
                "stale lease rejection changed the remote ref",
            )
            self.git.run(["-C", str(lease_owner), "fetch", "-q", "origin", "lease"])
            self.git.run([
                "-C", str(lease_owner), "push", "-q", "--force-with-lease", "origin", "lease"
            ])
            _assert(
                self.git.oid(self.fixture.primary_repo, "refs/heads/lease") == owner_oid,
                "fresh force-with-lease did not publish the exact object",
            )
            self.lease_before_force = owner_oid

        with self.recorder.probe("bundle", all_paths):
            self.git.run([
                "-C", str(lease_owner), "bundle", "create", str(self.bundle_path), "--all"
            ])
            self.git.run(["-C", str(lease_owner), "bundle", "verify", str(self.bundle_path)])
            recovered = self.root / "bundle-recovered"
            self.git.run(["clone", "-q", str(self.bundle_path), str(recovered)])
            self.git.run(["-C", str(recovered), "fsck", "--full", "--strict"])
            _assert(
                self.git.run([
                    "-C", str(recovered), "cat-file", "-e", self.lease_before_force
                ], check=False).returncode == 0,
                "verified bundle omitted the pre-force object",
            )

        with self.recorder.probe("force", ["direct"]):
            self.git.run([
                "-C", str(lease_owner), "switch", "-q", "-C", "force-candidate", "origin/main"
            ])
            forced_oid = _commit_file(
                self.git, lease_owner, "forced.txt", b"explicit force\n", "explicit force"
            )
            self.git.run([
                "-C", str(lease_owner), "push", "-q", "--force", "origin",
                "HEAD:refs/heads/lease",
            ])
            _assert(
                self.git.oid(self.fixture.primary_repo, "refs/heads/lease") == forced_oid,
                "explicit force published the wrong object id",
            )

        with self.recorder.probe("rollback-recovery", all_paths):
            recovered = self.root / "rollback-recovered"
            self.git.run(["clone", "-q", str(self.bundle_path), str(recovered)])
            self.git.run([
                "-C", str(recovered), "push", "-q", "--force", self.urls["direct"],
                f"{self.lease_before_force}:refs/heads/lease",
            ])
            _assert(
                self.git.oid(self.fixture.primary_repo, "refs/heads/lease")
                == self.lease_before_force,
                "bundle rollback did not restore the prior ref",
            )
            self.fixture.sync_mirror()
            for url in self.urls.values():
                advertised = self.git.text(["ls-remote", "--heads", url, "lease"])
                _assert(self.lease_before_force in advertised, "recovered ref was not readable")

        with self.recorder.probe("shallow-clone", all_paths):
            for path_id, url in self.urls.items():
                shallow = self.root / f"shallow-{path_id}"
                self._clone(url, shallow, "--depth=1")
                _assert(
                    self.git.text(["-C", str(shallow), "rev-parse", "--is-shallow-repository"])
                    == "true",
                    f"{path_id} depth clone is not shallow",
                )
                self.git.run(["-C", str(shallow), "fetch", "-q", "--unshallow", "origin"])
                _assert(
                    self.git.text(["-C", str(shallow), "rev-parse", "--is-shallow-repository"])
                    == "false",
                    f"{path_id} unshallow retained a shallow boundary",
                )
                self.git.run(["-C", str(shallow), "fsck", "--full", "--strict"])

        with self.recorder.probe("partial-clone", all_paths):
            for path_id, url in self.urls.items():
                partial = self.root / f"partial-{path_id}"
                self._clone(url, partial, "--filter=blob:none", "--no-checkout")
                _assert(
                    self.git.text(["-C", str(partial), "config", "--bool", "remote.origin.promisor"])
                    == "true",
                    f"{path_id} did not configure a promisor remote",
                )
                missing = self.git.text([
                    "-C", str(partial), "rev-list", "--objects", "--missing=print", "HEAD"
                ])
                _assert("?" in missing, f"{path_id} partial clone silently downloaded every blob")
                checkout = self.git.run(
                    ["-C", str(partial), "checkout", "-q"], check=False
                )
                if self.git.protocol_version == "0":
                    # A v0 lazy fetch asks for an object that was not in the
                    # ref advertisement. Keeping the hardened SHA-in-want
                    # policy prevents a guessed object behind a hidden
                    # refs/forkmesh/* ref from being disclosed. Protocol v2
                    # has the safe fetch negotiation used by the supported
                    # partial-clone path below.
                    error = checkout.stderr.decode("utf-8", "replace").lower()
                    _assert(
                        checkout.returncode != 0,
                        f"{path_id} protocol-v0 lazy fetch bypassed hidden-object policy",
                    )
                    _assert(
                        "unadvertised object" in error
                        or "could not fetch" in error
                        or "remote did not send" in error,
                        f"{path_id} protocol-v0 lazy-fetch rejection was not actionable",
                    )
                else:
                    _assert(
                        checkout.returncode == 0,
                        f"{path_id} protocol-v2 lazy blob fetch failed",
                    )
                    _assert(
                        (partial / "README.md").read_text(encoding="utf-8")
                        == "# stock Git conformance\n",
                        f"{path_id} lazy blob fetch changed content",
                    )
                self.git.run(["-C", str(partial), "fsck", "--connectivity-only"])
        if self.git.protocol_version == "0":
            self.recorder.rows["partial-clone"]["detail"] = (
                "expected safe protocol-v0 lazy-fetch rejection passed; use Git "
                "protocol v2 for partial clone"
            )

        with self.recorder.probe("submodules", all_paths):
            for path_id, url in self.urls.items():
                recursive = self.root / f"submodules-{path_id}"
                self._clone(url, recursive, "--recurse-submodules")
                _assert(
                    (recursive / "deps" / "fixture" / "submodule.txt").read_bytes()
                    == b"submodule object\n",
                    f"{path_id} recursive clone changed submodule contents",
                )
                _assert(
                    self.git.oid(recursive / "deps" / "fixture") == self.fixture.submodule_oid,
                    f"{path_id} recursive clone changed the gitlink object id",
                )

        with self.recorder.probe("integrity", all_paths):
            self._healthy(self.fixture.primary_repo)
            self._healthy(self.fixture.mirror_repo)
            primary_refs = _refs(self.git, self.fixture.primary_repo)
            mirror_refs = _refs(self.git, self.fixture.mirror_repo)
            _assert(primary_refs == mirror_refs, "mirror ref set differs after synchronization")
            bad_ref = self.root / "bad-ref.git"
            shutil.copytree(self.fixture.primary_repo, bad_ref)
            corrupt_ref = bad_ref / "refs" / "heads" / "corrupt"
            corrupt_ref.parent.mkdir(parents=True, exist_ok=True)
            corrupt_ref.write_text("not-an-object-id\n", encoding="ascii")
            _assert(
                self.git.run([
                    "--git-dir", str(bad_ref), "fsck", "--full", "--strict"
                ], check=False).returncode != 0,
                "ref corruption fixture was not detected",
            )
            bad_object = self.root / "bad-object.git"
            shutil.copytree(self.fixture.primary_repo, bad_object)
            self.git.run(["--git-dir", str(bad_object), "repack", "-a", "-d"])
            packs = sorted((bad_object / "objects" / "pack").glob("*.pack"))
            _assert(bool(packs), "object corruption fixture has no pack")
            pack = packs[0]
            data = bytearray(pack.read_bytes())
            _assert(len(data) > 64, "object corruption fixture pack is too small")
            data[len(data) // 2] ^= 0x01
            pack.chmod(pack.stat().st_mode | 0o200)
            pack.write_bytes(data)
            _assert(
                self.git.run([
                    "--git-dir", str(bad_object), "fsck", "--full", "--strict"
                ], check=False).returncode != 0,
                "object corruption fixture was not detected",
            )

        _assert(
            all(row["status"] == "pass" for row in self.recorder.rows.values()),
            "one or more promised operations did not run",
        )


def run_conformance(
    matrix: dict,
    *,
    git_executable: str,
    protocol_version: str,
    client_label: str,
    transport_mode: str,
) -> dict:
    with tempfile.TemporaryDirectory(prefix="forkmesh-git-conformance-") as directory:
        git = GitRunner(git_executable, protocol_version)
        return ConformanceSuite(
            matrix, git, Path(directory), client_label, transport_mode
        ).execute()


def _write_report(path: Path, report: dict) -> None:
    payload = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if str(path) == "-":
        sys.stdout.write(payload)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(payload, encoding="utf-8", newline="\n")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate", help="validate the versioned matrix and retained evidence")
    render = subparsers.add_parser("render", help="generate public matrix outputs")
    render.add_argument("--check", action="store_true", help="fail instead of writing stale outputs")
    run = subparsers.add_parser("run", help="run the offline stock-Git probes")
    run.add_argument("--git", default="git", help="Git executable, including a desktop client's bundled Git")
    run.add_argument("--protocol-version", choices=("0", "2"), default="0")
    run.add_argument("--client-label", default="system-git")
    run.add_argument(
        "--transport",
        choices=("auto", "http", "file"),
        default="auto",
        help="auto uses smart HTTP when loopback sockets are available",
    )
    run.add_argument("--output", type=Path, default=Path("artifacts/git-conformance.json"))
    attest_command = subparsers.add_parser(
        "attest",
        help="retain run reports, passing or failing, as public evidence and regenerate outputs",
    )
    attest_command.add_argument("reports", nargs="*", type=Path)
    attest_command.add_argument(
        "--drop", action="append", default=[], metavar="RUN_ID",
        help="retire a retained run whose client configuration is no longer tested",
    )
    attest_command.add_argument(
        "--date", type=date.fromisoformat, help="attestation date (default: today, UTC)"
    )
    gate = subparsers.add_parser(
        "gate", help="fail when any public support claim is failing, stale, or unverified"
    )
    gate.add_argument(
        "--report", action="append", default=[], type=Path,
        help="also count a fresh run report that has not been attested",
    )
    gate.add_argument("--as-of", type=date.fromisoformat, help="evaluation date (default: today, UTC)")
    gate.add_argument(
        "--operations-only", action="store_true",
        help="skip hand-recorded client certifications that a suite run cannot refresh",
    )
    return parser


def main() -> int:
    args = _parser().parse_args()
    try:
        matrix = load_matrix()
        validate_matrix(matrix)
        reports, attested = load_evidence(matrix)
        today = datetime.now(timezone.utc).date()
        if args.command == "attest":
            reports = attest(
                matrix, reports, [_json(path) for path in args.reports], drop=args.drop
            )
            attested = (args.date or today).isoformat()
            write_evidence(matrix, reports, attested)
            validate_evidence(matrix, reports, attested)
            summary = publish(matrix, reports, as_of=date.fromisoformat(attested))["summary"]
            print(
                f"Attested {len(reports)} retained runs: {summary['verified']} of "
                f"{summary['claims']} support claims verified, {summary['failing']} failing, "
                f"{summary['stale']} stale, {summary['unverified']} unverified"
            )
            return 0
        validate_evidence(matrix, reports, attested)
        if args.command == "validate":
            print(
                f"Git compatibility matrix is valid ({matrix['suite']['id']} "
                f"{matrix['suite']['version']}, {len(reports)} retained runs)"
            )
            return 0
        if args.command == "render":
            render_outputs(matrix, reports, attested, check=args.check)
            print("Git compatibility outputs are current" if args.check else "Generated Git compatibility outputs")
            return 0
        if args.command == "gate":
            if args.report:
                reports = attest(matrix, reports, [_json(path) for path in args.report])
            published = publish(matrix, reports, as_of=args.as_of or today)
            problems = degraded_claims(published, include_clients=not args.operations_only)
            summary = published["summary"]
            for problem in problems:
                print(f"git-conformance gate: {problem}", file=sys.stderr)
            print(
                f"Git compatibility gate {'failed' if problems else 'passed'} as of "
                f"{summary['asOf']}: {summary['verified']} of {summary['claims']} support "
                f"claims verified within {summary['freshnessDays']} days",
                file=sys.stderr if problems else sys.stdout,
            )
            return 1 if problems else 0
        report = run_conformance(
            matrix,
            git_executable=args.git,
            protocol_version=args.protocol_version,
            client_label=args.client_label,
            transport_mode=args.transport,
        )
        _write_report(args.output, report)
        summary = report["summary"]
        failed = [row for row in report["operations"] if row["status"] == "fail"]
        for row in failed:
            print(f"git-conformance: {row['id']} failed: {row['detail']}", file=sys.stderr)
        if summary["failure"] and not failed:
            print(f"git-conformance: {summary['failure']}", file=sys.stderr)
        print(
            f"Git conformance {summary['status']}: {summary['pass']} passed, "
            f"{summary['fail']} failed, {summary['notRun']} not run; report: {args.output}",
            file=sys.stderr if summary["status"] != "pass" else sys.stdout,
        )
        return 0 if summary["status"] == "pass" else 1
    except ConformanceError as exc:
        print(f"git-conformance: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
