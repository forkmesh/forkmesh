#!/usr/bin/env python3
"""ForkMesh protocol certification: profiles, adapter harness, evidence, and badges.

The program (conformance/certification/v1/program.json) groups protocol suite
vectors into profiles and names the defect classes each profile must detect.
``run`` drives any implementation through the language-neutral adapter
protocol and writes a report; ``attest`` retains reports as content-addressed
evidence; badges are derived from that evidence only, and expire when the
evidence ages past ``freshnessDays`` or the suite or profile changes.
See www/docs/protocol/certification.md.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import queue
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from datetime import date, datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Callable

ROOT = Path(__file__).resolve().parents[1]
PROGRAM_DIRECTORY = ROOT / "conformance" / "certification" / "v1"
PROGRAM_PATH = PROGRAM_DIRECTORY / "program.json"
REGISTRY_PATH = PROGRAM_DIRECTORY / "implementations.json"
RESULTS_PATH = PROGRAM_DIRECTORY / "results.json"
EVIDENCE_DIRECTORY = PROGRAM_DIRECTORY / "evidence"
MARKDOWN_PATH = ROOT / "www" / "docs" / "protocol" / "certification-registry.md"
PUBLIC_DIRECTORY = ROOT / "www" / "public" / "docs" / "certification"
PUBLIC_BADGES_PATH = PUBLIC_DIRECTORY / "badges.json"
PUBLIC_EVIDENCE_DIRECTORY = PUBLIC_DIRECTORY / "evidence"
PUBLIC_ORIGIN = "https://forkmesh.com"

PROGRAM_SCHEMA = "forkmesh-certification-program-v1"
REGISTRY_SCHEMA = "forkmesh-certification-registry-v1"
REPORT_SCHEMA = "forkmesh-certification-report-v1"
RESULTS_SCHEMA = "forkmesh-certification-results-v1"
BADGES_SCHEMA = "forkmesh-certification-badges-v1"
PLATFORMS = ("linux", "macos", "windows")
SLUG = re.compile(r"[a-z0-9][a-z0-9-]{0,62}")
EVIDENCE_LEAK = re.compile(r"(/home/|/Users/|/root/|/tmp/|[A-Za-z]:\\\\|file://|127\.0\.0\.1|localhost)")
HANDSHAKE_TIMEOUT_SECONDS = 30.0


class CertificationError(Exception):
    """The program, registry, evidence, or generated output is inconsistent."""


def _json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise CertificationError(f"cannot read {_display(path)}: {exc}") from None


def _display(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.name


def _digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _day(timestamp: str) -> date:
    return datetime.strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ").date()


def platform_name() -> str:
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform in {"win32", "cygwin"}:
        return "windows"
    return sys.platform


# --------------------------------------------------------------------------
# Vectors and profiles

def _input(*keys: str) -> Callable[[dict, dict], dict]:
    return lambda _data, case: {key: case[key] for key in keys}


def _validity(value_key: str | None = None) -> Callable[[dict], dict]:
    def expect(case: dict) -> dict:
        result = {"valid": case["expect"]["valid"]}
        if "error" in case["expect"]:
            result["error"] = case["expect"]["error"]
        if result["valid"] and value_key and value_key in case["expect"]:
            result["value"] = case["expect"][value_key]
        return result
    return expect


def _events_input(data: dict, case: dict) -> dict:
    by_id = {item["id"]: item for item in data["cases"]}
    return {
        "rawHex": case["rawHex"],
        "policy": data["policies"]["primary"],
        "context": [by_id[item]["rawHex"] for item in case["context"]],
    }


# group -> (adapter operation, request input, normalized expectation)
GROUPS: dict[str, tuple[str, Callable[[dict, dict], dict], Callable[[dict], dict]]] = {
    "canonical-json:parse": ("canonical.parse", _input("rawHex"), _validity()),
    "canonical-json:encode": ("canonical.encode", _input("value"), _validity("canonicalText")),
    "identifiers:repositories": ("identifier.repository", _input("verifiedRoot"), _validity("repositoryId")),
    "identifiers:policies": ("identifier.policy", _input("policy"), _validity("policyId")),
    "identifiers:objects": (
        "identifier.object",
        _input("repository", "family", "creator", "objectKey", "references"),
        _validity("objectId"),
    ),
    "identifiers:refsState": (
        "identifier.refs-state", _input("advertisementHex"), lambda case: {"value": case["expect"]},
    ),
    "identifiers:migration": (
        "identifier.migration",
        lambda _data, case: {key: value for key, value in case.items() if key not in {"id", "requirements", "expect"}},
        lambda case: {"value": case["expect"]["value"]},
    ),
    "signatures:ed25519": (
        "signature.ed25519", _input("publicKeyHex", "messageHex", "signatureHex"), _validity(),
    ),
    "signatures:canonicalStrings": (
        "signature.canonical-string",
        lambda data, case: {
            **{key: case[key] for key in ("scheme", "fields", "messageText", "publicKey", "signature") if key in case},
            "catalogV2Record": data["catalogV2Record"],
        },
        _validity(),
    ),
    "signatures:gitPushMessages": (
        "signature.git-push-message", lambda _data, case: dict(case["input"]), _validity("messageText"),
    ),
    "signatures:freshness": (
        "request.freshness", _input("ts", "nowMs"), lambda case: {"valid": case["expect"]["fresh"]},
    ),
    "events:cases": ("event.verify", _events_input, _validity("eventId")),
    "sessions:cases": (
        "session.parse",
        lambda data, case: {"token": case["token"], "nowMs": case["nowMs"], "routerPublicKey": data["routerPublicKey"]},
        _validity("claim"),
    ),
    "fold:scenarios": (
        "log.fold",
        lambda data, case: {"ndjson": case["ndjson"], "policy": data["policies"]["primary"]},
        lambda case: {"fold": case["expect"]},
    ),
}


def load_program() -> dict:
    return _json(PROGRAM_PATH)


def load_registry() -> dict:
    return _json(REGISTRY_PATH)


def _manifest(root: Path, program: dict) -> dict:
    return _json(root / program["protocolSuite"])


def load_vectors(root: Path, program: dict) -> dict[str, dict]:
    manifest = _manifest(root, program)
    return {Path(entry["path"]).stem: _json(root / entry["path"]) for entry in manifest["vectors"]}


def suite_binding(root: Path, program: dict) -> dict:
    manifest = _manifest(root, program)
    vectors = sorted(
        [entry["path"], hashlib.sha256((root / entry["path"]).read_bytes()).hexdigest()]
        for entry in manifest["vectors"]
    )
    return {"id": manifest["suite"], "version": manifest["version"], "vectorsSha256": _digest(vectors)}


def _classes_by_case(program: dict) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {}
    for detection in program["detectionClasses"]:
        for case_id in detection["cases"]:
            result.setdefault(case_id, []).append(detection["id"])
    return result


def _profile(program: dict, profile_id: str) -> dict:
    for profile in program["profiles"]:
        if profile["id"] == profile_id:
            return profile
    raise CertificationError(f"unknown profile {profile_id!r}")


def profile_cases(program: dict, profile_id: str, vectors: dict[str, dict]) -> list[dict]:
    profile = _profile(program, profile_id)
    classes = _classes_by_case(program)
    cases = []
    for group in profile["groups"]:
        operation, build_input, build_expect = GROUPS[group]
        vector_set, key = group.split(":", 1)
        data = vectors[vector_set]
        for case in data[key]:
            case_id = f"{vector_set}:{case['id']}"
            expect = build_expect(case)
            cases.append({
                "case": case_id,
                "operation": operation,
                "input": build_input(data, case),
                "expect": expect,
                "kind": "negative" if expect.get("valid") is False else "positive",
                "classes": [item for item in classes.get(case_id, []) if item in profile["detectionClasses"]],
            })
    return cases


def profile_binding(program: dict, profile_id: str) -> str:
    profile = _profile(program, profile_id)
    classes = {
        item["id"]: item["cases"] for item in program["detectionClasses"] if item["id"] in profile["detectionClasses"]
    }
    return _digest({"profile": profile, "classes": classes, "performance": program["performance"]})


def validate_program(program: dict, registry: dict, root: Path = ROOT) -> None:
    problems: list[str] = []
    if program.get("schema") != PROGRAM_SCHEMA:
        problems.append(f"program schema must be {PROGRAM_SCHEMA}")
    if not isinstance(program.get("freshnessDays"), int) or program["freshnessDays"] <= 0:
        problems.append("freshnessDays must be a positive integer")
    performance = program.get("performance") or {}
    if not all(isinstance(performance.get(key), int) and performance[key] > 0
               for key in ("maxCaseMilliseconds", "maxTotalSeconds")):
        problems.append("performance budgets must be positive integers")
    if not (root / str(program.get("governance", ""))).is_file():
        problems.append("governance document is missing")
    vectors = load_vectors(root, program)
    known_cases: dict[str, str] = {}
    for group in GROUPS:
        vector_set, key = group.split(":", 1)
        for case in vectors.get(vector_set, {}).get(key, []):
            known_cases[f"{vector_set}:{case['id']}"] = group
    for group in GROUPS:
        vector_set, key = group.split(":", 1)
        if key not in vectors.get(vector_set, {}):
            problems.append(f"harness group {group} has no vectors")

    class_ids = [item.get("id") for item in program.get("detectionClasses", [])]
    if len(set(class_ids)) != len(class_ids):
        problems.append("detection class ids must be unique")
    class_cases = {}
    for detection in program.get("detectionClasses", []):
        if not detection.get("cases"):
            problems.append(f"detection class {detection.get('id')} has no cases")
        for case_id in detection.get("cases", []):
            if case_id not in known_cases:
                problems.append(f"detection class {detection.get('id')} cites unknown case {case_id}")
        class_cases[detection.get("id")] = set(detection.get("cases", []))

    profile_ids = [item.get("id") for item in program.get("profiles", [])]
    if len(set(profile_ids)) != len(profile_ids) or not profile_ids:
        problems.append("profile ids must be unique and non-empty")
    for profile in program.get("profiles", []):
        groups = profile.get("groups", [])
        for group in groups:
            if group not in GROUPS:
                problems.append(f"profile {profile.get('id')} uses unknown group {group}")
        in_profile = {case_id for case_id, group in known_cases.items() if group in groups}
        for class_id in profile.get("detectionClasses", []):
            if class_id not in class_cases:
                problems.append(f"profile {profile.get('id')} requires unknown class {class_id}")
            elif not class_cases[class_id] & in_profile:
                problems.append(f"profile {profile.get('id')} cannot detect {class_id}: no case in its groups")
    gate = program.get("closureGate") or {}
    if gate.get("profile") not in profile_ids:
        problems.append("closureGate must name a profile")

    if registry.get("schema") != REGISTRY_SCHEMA:
        problems.append(f"registry schema must be {REGISTRY_SCHEMA}")
    seen: set[str] = set()
    for entry in registry.get("implementations", []):
        entry_id = entry.get("id")
        label = f"implementation {entry_id}"
        if not isinstance(entry_id, str) or not SLUG.fullmatch(entry_id) or entry_id in seen:
            problems.append(f"{label}: id must be a unique lowercase slug")
        seen.add(str(entry_id))
        for field in ("name", "maintainer", "source", "language"):
            if not isinstance(entry.get(field), str) or not entry[field]:
                problems.append(f"{label}: {field} is required")
        if not isinstance(entry.get("adapter"), list) or not entry["adapter"] or not all(
            isinstance(part, str) and part for part in entry["adapter"]
        ):
            problems.append(f"{label}: adapter must be a non-empty argument list")
        if not entry.get("platforms") or set(entry.get("platforms", [])) - set(PLATFORMS):
            problems.append(f"{label}: platforms must be drawn from {PLATFORMS}")
        if not entry.get("profiles") or set(entry.get("profiles", [])) - set(profile_ids):
            problems.append(f"{label}: profiles must name program profiles")
        if entry.get("independent") is True:
            independence = entry.get("independence") or {}
            if not all(isinstance(independence.get(key), str) and independence[key]
                       for key in ("reviewer", "date", "statement")):
                problems.append(f"{label}: independent implementations need a reviewer, date, and statement")
            if not str(entry.get("source", "")).startswith("https://"):
                problems.append(f"{label}: an independent implementation's source must be a public https URL")
        elif entry.get("independent") is False:
            if not entry.get("independenceNote"):
                problems.append(f"{label}: independenceNote must say why it is not independent")
        else:
            problems.append(f"{label}: independent must be true or false")
        for revocation in entry.get("revocations", []):
            if revocation.get("profile") not in entry.get("profiles", []) or not all(
                revocation.get(key) for key in ("date", "reason")
            ):
                problems.append(f"{label}: revocations need a claimed profile, date, and reason")
    if problems:
        raise CertificationError("; ".join(problems))


# --------------------------------------------------------------------------
# Adapter harness

def judge(expect: dict, response: Any) -> tuple[bool, str, str | None]:
    """Return (ok, detail, actual error code when it differs from the expected one)."""

    if not isinstance(response, dict):
        return False, "malformed response", None
    if response.get("unsupported") is True:
        return False, "operation unsupported by the implementation", None
    if not isinstance(response.get("valid"), bool):
        return False, "response has no boolean valid field", None
    valid, value = response["valid"], response.get("value")
    if "fold" in expect:
        want = expect["fold"]
        if not valid or not isinstance(value, dict):
            return False, f"fold rejected: {response.get('error')}", None
        telemetry = value.get("telemetry") if isinstance(value.get("telemetry"), dict) else {}
        mismatched = sorted(key for key, number in want["telemetry"].items() if telemetry.get(key) != number)
        if value.get("stateDigest") != want["stateDigest"]:
            return False, f"state digest {value.get('stateDigest')}", None
        if mismatched or value.get("rejectionReasons") != want["rejectionReasons"]:
            return False, "telemetry differs: " + ", ".join(mismatched or ["rejectionReasons"]), None
        return True, "", None
    if "valid" not in expect:
        if not valid:
            return False, f"rejected with {response.get('error')}", None
        return (True, "", None) if value == expect["value"] else (False, f"unexpected value {value!r}", None)
    if valid != expect["valid"]:
        shown = value if valid else response.get("error")
        return False, f"expected valid={str(expect['valid']).lower()}, got {shown!r}", None
    if valid and "value" in expect and value != expect["value"]:
        return False, f"unexpected value {value!r}", None
    if not valid and "error" in expect and response.get("error") != expect["error"]:
        return True, "", str(response.get("error"))
    return True, "", None


class _Adapter:
    def __init__(self, command: list[str], cwd: Path) -> None:
        self._stderr = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            command, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._stderr,
            text=True, encoding="utf-8", bufsize=1,
        )
        self.lines: queue.Queue[str | None] = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            if line.strip():
                self.lines.put(line)
        self.lines.put(None)

    def request(self, message: dict, timeout: float) -> tuple[Any, str]:
        assert self.process.stdin is not None
        try:
            # ASCII escapes carry the vectors' lone surrogates, which UTF-8 cannot.
            self.process.stdin.write(json.dumps(message, sort_keys=True) + "\n")
            self.process.stdin.flush()
        except OSError:
            return None, "adapter exited"
        try:
            line = self.lines.get(timeout=timeout)
        except queue.Empty:
            return None, f"timed out after {timeout:g}s"
        if line is None:
            return None, "adapter exited"
        try:
            return json.loads(line), ""
        except ValueError:
            return None, "response is not JSON"

    def close(self) -> None:
        try:
            if self.process.stdin:
                self.process.stdin.close()
        except OSError:
            pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self._stderr.close()


def _resolve(command: list[str]) -> list[str]:
    return [sys.executable if part == "{python}" else part for part in command]


def run_certification(
    program: dict,
    profile_id: str,
    command: list[str],
    *,
    implementation_id: str,
    root: Path = ROOT,
    case_timeout: float = 30.0,
) -> dict:
    vectors = load_vectors(root, program)
    cases = profile_cases(program, profile_id, vectors)
    suite = suite_binding(root, program)
    budget = program["performance"]
    started = _utc_now()
    adapter = _Adapter(_resolve(command), root)
    implementation = {"id": implementation_id, "name": "", "version": ""}
    stopped = ""
    try:
        hello, problem = adapter.request({
            "type": "hello",
            "protocol": program["adapterProtocol"],
            "suite": suite["id"],
            "suiteVersion": suite["version"],
        }, min(case_timeout, HANDSHAKE_TIMEOUT_SECONDS))
        if problem or not isinstance(hello, dict) or hello.get("type") != "hello":
            handshake, stopped = "incompatible", problem or "no hello response"
        elif hello.get("protocol") != program["adapterProtocol"]:
            handshake, stopped = "incompatible", f"adapter speaks {hello.get('protocol')!r}"
        elif suite["version"] not in (hello.get("suiteVersions") or []):
            handshake = "incompatible"
            stopped = f"adapter supports suite {hello.get('suiteVersions')}, harness runs {suite['version']}"
        else:
            handshake = "compatible"
        if isinstance(hello, dict) and isinstance(hello.get("implementation"), dict):
            implementation["name"] = str(hello["implementation"].get("name", ""))[:120]
            implementation["version"] = str(hello["implementation"].get("version", ""))[:60]
        results = []
        for case in cases:
            entry: dict[str, Any] = {
                "case": case["case"], "operation": case["operation"], "kind": case["kind"], "classes": case["classes"],
            }
            if stopped:
                entry.update(ok=False, elapsedMs=0, detail=f"not run: {stopped}")
                results.append(entry)
                continue
            began = time.monotonic()
            response, problem = adapter.request(
                {"type": "case", "id": case["case"], "operation": case["operation"], "input": case["input"]},
                case_timeout,
            )
            elapsed = int((time.monotonic() - began) * 1000)
            if problem:
                ok, detail, actual_error = False, problem, None
                stopped = problem
            elif isinstance(response, dict) and response.get("id") != case["case"]:
                ok, detail, actual_error = False, "response id does not match the request", None
            else:
                ok, detail, actual_error = judge(case["expect"], response)
            if ok and elapsed > budget["maxCaseMilliseconds"]:
                ok, detail = False, f"exceeded the {budget['maxCaseMilliseconds']} ms case budget"
            entry.update(ok=ok, elapsedMs=elapsed)
            if detail:
                entry["detail"] = detail[:300]
            if actual_error is not None:
                entry["expectedError"] = case["expect"]["error"]
                entry["actualError"] = actual_error[:120]
            results.append(entry)
    finally:
        adapter.close()
    return build_report(
        program, profile_id, suite, implementation, command, handshake, stopped if handshake != "compatible" else "",
        results, started, _utc_now(),
    )


def _detection(profile: dict, results: list[dict]) -> dict:
    detection = {}
    for class_id in profile["detectionClasses"]:
        covered = [item for item in results if class_id in item["classes"]]
        detection[class_id] = {
            "cases": len(covered), "failed": [item["case"] for item in covered if not item["ok"]],
        }
    return detection


def _performance(program: dict, results: list[dict]) -> dict:
    budget = program["performance"]
    total = sum(item["elapsedMs"] for item in results)
    slowest = max(results, key=lambda item: item["elapsedMs"], default={"case": "", "elapsedMs": 0})
    return {
        "totalMilliseconds": total,
        "slowestCase": slowest["case"],
        "slowestMilliseconds": slowest["elapsedMs"],
        "maxCaseMilliseconds": budget["maxCaseMilliseconds"],
        "maxTotalSeconds": budget["maxTotalSeconds"],
        "withinBudget": total <= budget["maxTotalSeconds"] * 1000
        and slowest["elapsedMs"] <= budget["maxCaseMilliseconds"],
    }


def _summary(results: list[dict], handshake: str, performance: dict) -> dict:
    passed = sum(1 for item in results if item["ok"])
    return {
        "cases": len(results),
        "passed": passed,
        "failed": len(results) - passed,
        "codeDeviations": sum(1 for item in results if "actualError" in item),
        "result": "pass" if results and passed == len(results) and handshake == "compatible"
        and performance["withinBudget"] else "fail",
    }


def build_report(
    program: dict, profile_id: str, suite: dict, implementation: dict, command: list[str], handshake: str,
    handshake_detail: str, results: list[dict], started: str, finished: str,
) -> dict:
    profile = _profile(program, profile_id)
    performance = _performance(program, results)
    return {
        "schema": REPORT_SCHEMA,
        "program": {
            "version": program["version"], "profile": profile_id, "profileSha256": profile_binding(program, profile_id),
        },
        "suite": suite,
        "implementation": implementation,
        "adapter": {
            "protocol": program["adapterProtocol"], "command": command, "handshake": handshake,
            **({"detail": handshake_detail[:300]} if handshake_detail else {}),
        },
        "runner": {"platform": platform_name(), "python": platform.python_version()},
        "startedAt": started,
        "finishedAt": finished,
        "cases": results,
        "detection": _detection(profile, results),
        "performance": performance,
        "summary": _summary(results, handshake, performance),
    }


# --------------------------------------------------------------------------
# Evidence

def report_id(report: dict) -> str:
    return _digest(report)[:16]


def _evidence_text(report: dict) -> str:
    return json.dumps(report, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def is_current(program: dict, report: dict, root: Path = ROOT) -> bool:
    profile_id = report["program"]["profile"]
    return (
        report["program"]["version"] == program["version"]
        and any(item["id"] == profile_id for item in program["profiles"])
        and report["program"]["profileSha256"] == profile_binding(program, profile_id)
        and report["suite"] == suite_binding(root, program)
    )


def validate_report(program: dict, report: Any, root: Path = ROOT) -> None:
    def fail(message: str) -> None:
        raise CertificationError(f"report {report_id(report) if isinstance(report, dict) else '?'}: {message}")

    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        raise CertificationError(f"report schema must be {REPORT_SCHEMA}")
    for field in ("program", "suite", "implementation", "adapter", "runner", "performance", "summary", "detection"):
        if not isinstance(report.get(field), dict):
            fail(f"{field} is required")
    if not isinstance(report.get("cases"), list):
        fail("cases must be a list")
    if not SLUG.fullmatch(str(report["implementation"].get("id", ""))):
        fail("implementation id must be a lowercase slug")
    if report["runner"].get("platform") not in PLATFORMS:
        fail(f"runner platform must be one of {PLATFORMS}")
    try:
        if _day(report["startedAt"]) > _day(report["finishedAt"]) or report["startedAt"] > report["finishedAt"]:
            fail("finishedAt precedes startedAt")
    except (KeyError, TypeError, ValueError):
        fail("startedAt and finishedAt must be UTC timestamps")
    for item in report["cases"]:
        if not isinstance(item, dict) or not isinstance(item.get("ok"), bool) or not isinstance(item.get("case"), str):
            fail("every case needs a case id and boolean ok")
        if not isinstance(item.get("classes"), list) or not isinstance(item.get("elapsedMs"), int):
            fail(f"{item.get('case')} needs classes and elapsedMs")
    if EVIDENCE_LEAK.search(json.dumps(report, ensure_ascii=False)):
        fail("contains a local path, file URL, or loopback address")
    performance = report["performance"]
    expected_summary = _summary(report["cases"], report["adapter"].get("handshake"), performance)
    if report["summary"] != expected_summary:
        fail("summary does not match its cases")
    if performance.get("totalMilliseconds") != sum(item["elapsedMs"] for item in report["cases"]):
        fail("performance does not match its cases")
    if is_current(program, report, root):
        profile_id = report["program"]["profile"]
        expected = profile_cases(program, profile_id, load_vectors(root, program))
        if [(item["case"], item["classes"]) for item in report["cases"]] != [
            (item["case"], item["classes"]) for item in expected
        ]:
            fail("case list differs from the profile it claims")
        if report["detection"] != _detection(_profile(program, profile_id), report["cases"]):
            fail("detection summary does not match its cases")
        if performance != _performance(program, report["cases"]):
            fail("performance budget does not match the program")


def _key(report: dict) -> tuple[str, str, str]:
    return (report["implementation"]["id"], report["program"]["profile"], report["runner"]["platform"])


def attest(
    program: dict, registry: dict, reports: dict[str, dict], new_reports: list[dict], *, drop: list[str] = (),
    root: Path = ROOT,
) -> dict[str, dict]:
    retained = {run_id: report for run_id, report in reports.items() if run_id not in set(drop)}
    unknown = set(drop) - set(reports)
    if unknown:
        raise CertificationError(f"cannot drop unknown runs: {sorted(unknown)}")
    entries = {entry["id"]: entry for entry in registry["implementations"]}
    for report in new_reports:
        validate_report(program, report, root)
        entry = entries.get(report["implementation"]["id"])
        if entry is None:
            raise CertificationError(
                f"{report['implementation']['id']} is not registered in {_display(REGISTRY_PATH)}"
            )
        if report["program"]["profile"] not in entry["profiles"]:
            raise CertificationError(f"{entry['id']} does not claim profile {report['program']['profile']}")
        if report["runner"]["platform"] not in entry["platforms"]:
            raise CertificationError(f"{entry['id']} does not list platform {report['runner']['platform']}")
        if report["adapter"].get("command") != entry["adapter"]:
            raise CertificationError(
                f"{entry['id']} report was not produced by its registered adapter command"
            )
        newest = next((item for item in retained.values() if _key(item) == _key(report)), None)
        if newest is not None and newest["finishedAt"] > report["finishedAt"]:
            continue
        retained = {run_id: item for run_id, item in retained.items() if _key(item) != _key(report)}
        retained[report_id(report)] = report
    return dict(sorted(retained.items()))


def results_document(reports: dict[str, dict], attested: str) -> dict:
    runs = [
        {
            "runId": run_id,
            "implementation": report["implementation"]["id"],
            "profile": report["program"]["profile"],
            "platform": report["runner"]["platform"],
            "finishedAt": report["finishedAt"],
            "suiteVersion": report["suite"]["version"],
            "result": report["summary"]["result"],
            "cases": report["summary"]["cases"],
            "failed": report["summary"]["failed"],
        }
        for run_id, report in reports.items()
    ]
    runs.sort(key=lambda item: (item["implementation"], item["profile"], item["platform"]))
    return {"schema": RESULTS_SCHEMA, "attested": attested, "runs": runs}


def load_evidence(program: dict, root: Path = ROOT) -> tuple[dict[str, dict], str]:
    results_path = root / RESULTS_PATH.relative_to(ROOT)
    evidence_directory = root / EVIDENCE_DIRECTORY.relative_to(ROOT)
    if not results_path.exists():
        return {}, date.today().isoformat()
    results = _json(results_path)
    reports = {}
    for path in sorted(evidence_directory.glob("*.json")) if evidence_directory.exists() else []:
        report = _json(path)
        validate_report(program, report, root)
        if report_id(report) != path.stem:
            raise CertificationError(f"{_display(path)} is not named by its content address {report_id(report)}")
        reports[path.stem] = report
    if results_document(reports, results.get("attested", "")) != results:
        raise CertificationError(
            f"{_display(results_path)} does not match retained evidence; run python3 tools/protocol_certification.py attest"
        )
    return reports, results["attested"]


# --------------------------------------------------------------------------
# Badges

def derive_badges(program: dict, registry: dict, reports: dict[str, dict], *, as_of: date, root: Path = ROOT) -> dict:
    freshness = program["freshnessDays"]
    badges = []
    for entry in registry["implementations"]:
        for profile_id in entry["profiles"]:
            runs = []
            for platform_id in entry["platforms"]:
                matching = [
                    (run_id, report) for run_id, report in reports.items()
                    if _key(report) == (entry["id"], profile_id, platform_id)
                ]
                if matching:
                    runs.append(max(matching, key=lambda pair: pair[1]["finishedAt"]))
            badge: dict[str, Any] = {
                "implementation": entry["id"],
                "name": entry["name"],
                "profile": profile_id,
                "independent": entry["independent"],
                "platforms": [
                    {"platform": report["runner"]["platform"], "runId": run_id,
                     "testedAt": report["finishedAt"][:10], "result": report["summary"]["result"]}
                    for run_id, report in runs
                ],
                "reproduce": (
                    f"python3 tools/protocol_certification.py run --implementation {entry['id']} "
                    f"--profile {profile_id}"
                ),
            }
            revocation = next((item for item in entry.get("revocations", []) if item["profile"] == profile_id), None)
            tested = [_day(report["finishedAt"]) for _run_id, report in runs]
            if runs:
                badge["evidence"] = f"/docs/certification/evidence/{runs[0][0]}.json"
                badge["testedAt"] = min(tested).isoformat()
                badge["suiteVersion"] = runs[0][1]["suite"]["version"]
            missing = sorted(set(entry["platforms"]) - {report["runner"]["platform"] for _run_id, report in runs})
            if revocation:
                badge.update(status="revoked", reason=f"revoked {revocation['date']}: {revocation['reason']}")
            elif missing:
                badge.update(status="unverified", reason="no retained run on " + ", ".join(missing))
            elif any(report["summary"]["result"] != "pass" for _run_id, report in runs):
                badge.update(status="failing", reason="the newest retained run failed")
            elif not all(is_current(program, report, root) for _run_id, report in runs):
                badge.update(status="expired", reason="the suite vectors or profile changed after testing")
            else:
                expires = min(tested) + timedelta(days=freshness)
                badge["expiresAt"] = expires.isoformat()
                if as_of > expires:
                    badge.update(status="expired", reason=f"evidence is older than {freshness} days")
                else:
                    badge.update(status="certified", reason="")
            badges.append(badge)
    gate = program["closureGate"]
    independent = sorted(
        badge["implementation"] for badge in badges
        if badge["profile"] == gate["profile"] and badge["status"] == "certified"
        and (badge["independent"] or not gate["requiresIndependentImplementation"])
    )
    return {
        "schema": BADGES_SCHEMA,
        "program": {"version": program["version"], "governance": f"{PUBLIC_ORIGIN}/docs/library/protocol/certification/"},
        "suite": suite_binding(root, program),
        "asOf": as_of.isoformat(),
        "freshnessDays": freshness,
        "badges": badges,
        "closureGate": {**gate, "certified": independent, "met": bool(independent)},
    }


def render_markdown(program: dict, published: dict, vectors: dict[str, dict]) -> str:
    lines = [
        "<!-- Generated by tools/protocol_certification.py render. Do not edit. -->",
        "",
        "# Protocol certification registry",
        "",
        f"Evaluated as of {published['asOf']} against suite `{published['suite']['version']}` · "
        f"badges stay valid for {published['freshnessDays']} days after testing · "
        "[Program and governance](certification.md)",
        "",
    ]
    gate = published["closureGate"]
    if gate["met"]:
        lines.append(f"Closure gate met: {', '.join(gate['certified'])} holds a current `{gate['profile']}` badge.")
    else:
        lines.append(
            f"Closure gate **not met**: no independent implementation holds a current `{gate['profile']}` badge yet."
        )
    lines += [
        "",
        "| Implementation | Independent | Profile | Status | Tested | Expires | Evidence |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for badge in published["badges"]:
        evidence = f"[{badge['evidence'].rsplit('/', 1)[-1][:-5]}]({PUBLIC_ORIGIN}{badge['evidence']})" \
            if badge.get("evidence") else "—"
        status = badge["status"] + (f" — {badge['reason']}" if badge["reason"] else "")
        lines.append(
            f"| {badge['name']} (`{badge['implementation']}`) | {'yes' if badge['independent'] else 'no'} | "
            f"`{badge['profile']}` | {status} | {badge.get('testedAt', '—')} | {badge.get('expiresAt', '—')} | "
            f"{evidence} |"
        )
    lines += [
        "",
        "The hosted badge re-checks expiry on every request, so an embedded badge turns `expired` on its own.",
        "",
        "## Embedding a badge",
        "",
    ]
    for badge in published["badges"]:
        path = f"{PUBLIC_ORIGIN}/docs/certification/badges/{badge['implementation']}/{badge['profile']}"
        lines += [
            f"`{badge['implementation']}` · `{badge['profile']}` — reproduce with `{badge['reproduce']}`",
            "",
            "```markdown",
            f"[![ForkMesh {badge['profile']} certification]({path}.svg)]({path})",
            "```",
            "",
        ]
    lines += ["## Detection coverage", "", "| Profile | Class | Cases |", "| --- | --- | --- |"]
    for profile in program["profiles"]:
        cases = profile_cases(program, profile["id"], vectors)
        for class_id in profile["detectionClasses"]:
            count = sum(1 for case in cases if class_id in case["classes"])
            lines.append(f"| `{profile['id']}` | {class_id} | {count} |")
    return "\n".join(lines) + "\n"


def generated_outputs(
    program: dict, registry: dict, reports: dict[str, dict], attested: str, root: Path = ROOT,
) -> dict[Path, str]:
    published = derive_badges(program, registry, reports, as_of=date.fromisoformat(attested), root=root)
    outputs = {
        root / MARKDOWN_PATH.relative_to(ROOT): render_markdown(program, published, load_vectors(root, program)),
        root / PUBLIC_BADGES_PATH.relative_to(ROOT): json.dumps(published, indent=2, ensure_ascii=False) + "\n",
    }
    for run_id, report in reports.items():
        outputs[root / PUBLIC_EVIDENCE_DIRECTORY.relative_to(ROOT) / f"{run_id}.json"] = _evidence_text(report)
    return outputs


def _sync_files(outputs: dict[Path, str], directory: Path, *, check: bool) -> list[str]:
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
    program: dict, registry: dict, reports: dict[str, dict], attested: str, *, check: bool, root: Path = ROOT,
) -> None:
    stale = _sync_files(
        generated_outputs(program, registry, reports, attested, root),
        root / PUBLIC_EVIDENCE_DIRECTORY.relative_to(ROOT),
        check=check,
    )
    if stale:
        raise CertificationError(
            "generated certification output is stale: " + ", ".join(stale)
            + "; run python3 tools/protocol_certification.py render"
        )


def write_evidence(program: dict, registry: dict, reports: dict[str, dict], attested: str, root: Path = ROOT) -> None:
    evidence_directory = root / EVIDENCE_DIRECTORY.relative_to(ROOT)
    outputs = {evidence_directory / f"{run_id}.json": _evidence_text(report) for run_id, report in reports.items()}
    evidence_directory.mkdir(parents=True, exist_ok=True)
    _sync_files(outputs, evidence_directory, check=False)
    (root / RESULTS_PATH.relative_to(ROOT)).write_text(
        json.dumps(results_document(reports, attested), indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    render_outputs(program, registry, reports, attested, check=False, root=root)


# --------------------------------------------------------------------------
# Command line

def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="validate the program, registry, and retained evidence")
    render = commands.add_parser("render", help="regenerate the registry page and hosted badge data")
    render.add_argument("--check", action="store_true", help="fail instead of writing stale outputs")
    run = commands.add_parser("run", help="run an implementation's adapter against a profile")
    run.add_argument("--profile", required=True)
    run.add_argument("--implementation", required=True, help="registered id, or a new slug with --command")
    run.add_argument("--command", dest="adapter_command", help="adapter command line for an unregistered implementation")
    run.add_argument("--output", type=Path, default=Path("artifacts/protocol-certification.json"))
    run.add_argument("--case-timeout", type=float, default=30.0)
    attest_command = commands.add_parser("attest", help="retain run reports as evidence and regenerate outputs")
    attest_command.add_argument("reports", nargs="*", type=Path)
    attest_command.add_argument("--drop", action="append", default=[], metavar="RUN_ID")
    attest_command.add_argument("--date", type=date.fromisoformat, help="attestation date (default: today, UTC)")
    gate = commands.add_parser(
        "gate", help="validate, check outputs, and re-run every registered in-repository adapter now"
    )
    gate.add_argument("--case-timeout", type=float, default=30.0)
    status = commands.add_parser("status", help="print badge status as of a date")
    status.add_argument("--as-of", type=date.fromisoformat)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        program, registry = load_program(), load_registry()
        validate_program(program, registry)
        reports, attested = load_evidence(program)
        today = datetime.now(timezone.utc).date()
        entries = {entry["id"]: entry for entry in registry["implementations"]}
        if args.command == "validate":
            print(f"Certification program {program['version']} is valid ({len(reports)} retained runs)")
            return 0
        if args.command == "render":
            render_outputs(program, registry, reports, attested, check=args.check)
            print("Certification outputs are current" if args.check else "Generated certification outputs")
            return 0
        if args.command == "attest":
            reports = attest(
                program, registry, reports, [_json(path) for path in args.reports], drop=args.drop
            )
            attested = (args.date or today).isoformat()
            write_evidence(program, registry, reports, attested)
            published = derive_badges(program, registry, reports, as_of=date.fromisoformat(attested))
            for badge in published["badges"]:
                print(f"{badge['implementation']} {badge['profile']}: {badge['status']}")
            return 0
        if args.command == "status":
            published = derive_badges(program, registry, reports, as_of=args.as_of or today)
            print(json.dumps(published, indent=2))
            return 0
        if args.command == "run":
            if args.adapter_command:
                command = shlex.split(args.adapter_command)
            elif args.implementation in entries:
                command = entries[args.implementation]["adapter"]
            else:
                raise CertificationError(f"{args.implementation} is not registered; pass --command")
            report = run_certification(
                program, args.profile, command, implementation_id=args.implementation, case_timeout=args.case_timeout
            )
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(_evidence_text(report), encoding="utf-8", newline="\n")
            summary = report["summary"]
            failed_classes = [name for name, item in report["detection"].items() if item["failed"]]
            print(
                f"{args.implementation} {args.profile}: {summary['result']} — {summary['passed']} of "
                f"{summary['cases']} cases, {summary['codeDeviations']} code deviations"
                + (f"; failing classes: {', '.join(failed_classes)}" if failed_classes else "")
                + f"; report: {args.output}",
                file=sys.stdout if summary["result"] == "pass" else sys.stderr,
            )
            return 0 if summary["result"] == "pass" else 1
        render_outputs(program, registry, reports, attested, check=True)
        failures = []
        for entry in registry["implementations"]:
            if entry["source"].startswith("https://"):
                continue
            for profile_id in entry["profiles"]:
                report = run_certification(
                    program, profile_id, entry["adapter"], implementation_id=entry["id"],
                    case_timeout=args.case_timeout,
                )
                summary = report["summary"]
                print(f"{entry['id']} {profile_id}: {summary['result']} ({summary['passed']}/{summary['cases']})")
                if summary["result"] != "pass":
                    failures.append(f"{entry['id']} {profile_id}")
        gate = derive_badges(program, registry, reports, as_of=today)["closureGate"]
        print(
            f"Closure gate ({gate['profile']}, independent implementation): "
            + ("met by " + ", ".join(gate["certified"]) if gate["met"] else "not met")
        )
        if failures:
            print("protocol-certification gate failed: " + ", ".join(failures), file=sys.stderr)
            return 1
        return 0
    except CertificationError as exc:
        print(f"protocol-certification: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
