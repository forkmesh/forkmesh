#!/usr/bin/env python3
"""SDK-light client and contract runner for ForkMesh's public integration contract.

One standard-library file that third parties may vendor next to their own
tooling. It implements the v1 contract in
``conformance/integrations/v1/contract.json`` (documented in
``www/docs/integrations.md``):

* ``discover`` probes a hosted or self-hosted service and prints which contract
  resources it serves, with a version-skew verdict;
* ``contract`` runs the executable contract and prints a non-secret report;
* ``watch`` turns stable HTTP resources into an at-least-once NDJSON event
  stream with per-subject sequences;
* ``deliver`` posts those events to a webhook with HMAC signatures, bounded
  retries, and a replayable dead-letter file;
* ``fixture`` serves an offline stand-in for both services, including the
  previous/next version-skew profiles, so tests and examples need no network.

``EventLedger`` is the consumer half: it absorbs duplicate, delayed, missing,
and out-of-order deliveries and converges on the same per-subject state.
"""

from __future__ import annotations

import argparse
from collections import OrderedDict
from datetime import date, datetime, timezone
from email.utils import parsedate_to_datetime
import hashlib
import hmac
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import platform
import random
import re
import sys
import threading
import time
from typing import Any, Callable, Iterable, Mapping
from urllib import error as urlerror
from urllib import request as urlrequest
from urllib.parse import urlsplit, urlunsplit


TOOL_VERSION = "1.0.0"
ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT / "conformance" / "integrations" / "v1" / "contract.json"
REPORT_SCHEMA = "forkmesh-integration-contract-report-v1"
EVENT_SCHEMA = "forkmesh-event-v1"
CLI_SCHEMA = "forkmesh-cli-v1"
WATCH_STATE_SCHEMA = "forkmesh-watch-state-v1"
WEBHOOK_SCHEME = "forkmesh-webhook-v1"
WEBHOOK_TOLERANCE_SECONDS = 300
USER_AGENT = "forkmesh-integration/%s (+https://forkmesh.com/docs)" % TOOL_VERSION

EXIT_OK = 0
EXIT_FAILED = 1
EXIT_USAGE = 2
EXIT_SKEW = 3

SERVICES = ("hosted", "self-hosted")
DEFAULT_BASE_URLS = {
    "hosted": "https://app.forkmesh.com",
    "self-hosted": "http://127.0.0.1:8791",
}
RETRY_STATUSES = frozenset({408, 425, 429, 500, 502, 503, 504})
MIN_POLL_SECONDS = {"hosted": 30, "self-hosted": 5}
GAP_TIMEOUT_SECONDS = 120
REQUIRED_POLICIES = (
    "discovery", "authentication", "pagination", "filtering", "idempotency",
    "ordering", "retries", "signatures", "rateLimits", "errors", "deprecation",
)
REQUIRED_CLI_COMMANDS = {
    "validate", "discover", "contract", "watch", "deliver", "fixture",
}
REQUIRED_WEBHOOK_HEADERS = {
    "ForkMesh-Webhook-Id", "ForkMesh-Webhook-Timestamp",
    "ForkMesh-Webhook-Signature", "ForkMesh-Webhook-Attempt",
    "ForkMesh-Event-Type",
}
REQUIRED_EXAMPLE_CATEGORIES = {
    "migration", "automation", "discovery", "deployment", "analytics",
    "moderation",
}
FIELD_TYPES = {
    "boolean": lambda value: isinstance(value, bool),
    "string": lambda value: isinstance(value, str),
    "integer": lambda value: isinstance(value, int) and not isinstance(value, bool),
    "number": lambda value: (
        isinstance(value, (int, float)) and not isinstance(value, bool)),
    "array": lambda value: isinstance(value, list),
    "object": lambda value: isinstance(value, dict),
}
# Catalog records also carry live telemetry (CPU, memory, disk). Events only
# fold the identity and state fields so an idle repository stays quiet.
CATALOG_EVENT_FIELDS = (
    "description", "visibility", "updatedAt", "commit", "stateHash",
    "cloneUrl", "liveHost", "cloneOnline", "termsFlagged",
)
ITEM_CHECK_LIMIT = 50


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")


def _now_ms() -> int:
    return int(time.time() * 1000)


def _utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z")


def redact_url(url: str) -> str:
    """Drop userinfo, query, and fragment so reports never carry credentials."""
    parts = urlsplit(str(url or ""))
    host = parts.hostname or ""
    if parts.port:
        host = "%s:%d" % (host, parts.port)
    return urlunsplit((parts.scheme, host, parts.path.rstrip("/"), "", ""))


def emit(line: Mapping[str, Any], stream=None) -> None:
    stream = stream or sys.stdout
    stream.write(json.dumps(line, sort_keys=True, separators=(",", ":")) + "\n")
    stream.flush()


# ---------------------------------------------------------------- contract


def load_contract(path: str | os.PathLike | None = None) -> dict:
    candidates = (
        [Path(path)] if path
        else [CONTRACT_PATH, Path(__file__).with_name("contract.json")]
    )
    for candidate in candidates:
        if candidate.is_file():
            return json.loads(candidate.read_text(encoding="utf-8"))
    raise FileNotFoundError("integration contract not found; pass --contract")


def _check_type_map(label: str, mapping: Any, errors: list[str]) -> None:
    if not isinstance(mapping, dict) or not mapping:
        errors.append("%s must be a non-empty object" % label)
        return
    for field, kind in mapping.items():
        if kind not in FIELD_TYPES:
            errors.append("%s.%s has unknown type %r" % (label, field, kind))


def validate_contract(contract: Mapping[str, Any]) -> None:
    """Raise ValueError listing every structural problem in a manifest."""
    errors: list[str] = []
    if contract.get("schemaVersion") != 1:
        errors.append("schemaVersion must be 1")
    suite = contract.get("suite") or {}
    for key in ("id", "version", "updated", "tool", "documentation"):
        if not isinstance(suite.get(key), str) or not suite.get(key):
            errors.append("suite.%s is required" % key)
    for key, expected in (
        ("reportSchema", REPORT_SCHEMA), ("eventSchema", EVENT_SCHEMA),
        ("cliSchema", CLI_SCHEMA),
    ):
        if suite.get(key) != expected:
            errors.append("suite.%s must be %s" % (key, expected))
    versioning = contract.get("versioning") or {}
    suite_version = parse_version(suite.get("version"))
    if not suite_version or suite_version[0] != versioning.get("contractMajor"):
        errors.append("suite.version major must equal versioning.contractMajor")
    if not parse_version(versioning.get("serviceMinimum")):
        errors.append("versioning.serviceMinimum must be a version")
    if set(versioning.get("skewProfiles") or []) != {"previous", "current", "next"}:
        errors.append("versioning.skewProfiles must be previous, current, next")

    services = {item.get("id") for item in contract.get("services") or []}
    if services != set(SERVICES):
        errors.append("services must be exactly %s" % ", ".join(SERVICES))
    auth_ids = {item.get("id") for item in contract.get("authentication") or []}
    for item in contract.get("authentication") or []:
        if not item.get("summary"):
            errors.append("authentication %s needs a summary" % item.get("id"))

    policies = contract.get("policies") or {}
    for name in REQUIRED_POLICIES:
        if not (policies.get(name) or {}).get("summary"):
            errors.append("policies.%s.summary is required" % name)
    retries = policies.get("retries") or {}
    if set(retries.get("retryStatuses") or []) != set(RETRY_STATUSES):
        errors.append("policies.retries.retryStatuses drifted from the client")
    polls = (policies.get("rateLimits") or {}).get("minimumPollSeconds") or {}
    if polls != MIN_POLL_SECONDS:
        errors.append("policies.rateLimits.minimumPollSeconds drifted from the client")

    interfaces = contract.get("interfaces") or {}
    seen_ids: set[str] = set()
    kinds_by_service: dict[str, set[str]] = {service: set() for service in SERVICES}
    for resource in interfaces.get("http") or []:
        rid = resource.get("id")
        label = "interfaces.http[%s]" % rid
        if not rid or rid in seen_ids:
            errors.append("%s id must be present and unique" % label)
        seen_ids.add(rid)
        if resource.get("service") not in SERVICES:
            errors.append("%s service is unknown" % label)
        if resource.get("auth") not in auth_ids:
            errors.append("%s auth is not a declared scheme" % label)
        if resource.get("method") not in ("GET", "HEAD", "POST"):
            errors.append("%s method is unsupported" % label)
        if not str(resource.get("path") or "").startswith("/"):
            errors.append("%s path must start with /" % label)
        if resource.get("stability") not in ("stable", "beta", "deprecated"):
            errors.append("%s stability is unknown" % label)
        statuses = resource.get("expectStatus")
        if not statuses or not all(isinstance(code, int) for code in statuses):
            errors.append("%s expectStatus must list integers" % label)
        _check_type_map(label + ".requiredFields",
                        resource.get("requiredFields"), errors)
        if resource.get("itemFields") is not None:
            _check_type_map(label + ".itemFields",
                            resource.get("itemFields"), errors)
            if not resource.get("itemsField"):
                errors.append("%s itemFields needs itemsField" % label)
        kind = resource.get("kind")
        if kind not in ("resource", "error"):
            errors.append("%s kind must be resource or error" % label)
        if kind == "error" and "error" not in (resource.get("requiredFields") or {}):
            errors.append("%s error probes must require the error field" % label)
        if resource.get("service") in kinds_by_service and kind:
            kinds_by_service[resource["service"]].add(kind)
    for service, kinds in kinds_by_service.items():
        if kinds != {"resource", "error"}:
            errors.append("service %s needs resource and error probes" % service)

    events = interfaces.get("events") or {}
    if events.get("schema") != EVENT_SCHEMA:
        errors.append("interfaces.events.schema must be %s" % EVENT_SCHEMA)
    _check_type_map("interfaces.events.requiredFields",
                    events.get("requiredFields"), errors)
    for item in events.get("types") or []:
        if item.get("service") not in SERVICES or not item.get("type"):
            errors.append("event type %r is incomplete" % item)
    cli = interfaces.get("cli") or {}
    commands = {item.get("id") for item in cli.get("commands") or []}
    if not REQUIRED_CLI_COMMANDS <= commands:
        errors.append("interfaces.cli.commands is missing %s" % sorted(
            REQUIRED_CLI_COMMANDS - commands))
    if set((cli.get("exitCodes") or {})) != {"0", "1", "2", "3"}:
        errors.append("interfaces.cli.exitCodes must define 0-3")
    outbound = (interfaces.get("webhooks") or {}).get("outbound") or {}
    if outbound.get("schema") != WEBHOOK_SCHEME:
        errors.append("webhooks.outbound.schema must be %s" % WEBHOOK_SCHEME)
    if set(outbound.get("headers") or []) != REQUIRED_WEBHOOK_HEADERS:
        errors.append("webhooks.outbound.headers drifted from the client")

    notice_days = int((policies.get("deprecation") or {}).get("noticeDays") or 0)
    for item in contract.get("deprecations") or []:
        try:
            announced = date.fromisoformat(item["announced"])
            sunset = date.fromisoformat(item["sunset"])
        except (KeyError, TypeError, ValueError):
            errors.append("deprecation %r needs announced and sunset dates" % item)
            continue
        if (sunset - announced).days < notice_days:
            errors.append("deprecation %s sunsets before noticeDays" % item.get("id"))
        if not item.get("replacement"):
            errors.append("deprecation %s needs a replacement" % item.get("id"))

    categories: set[str] = set()
    for example in contract.get("examples") or []:
        categories.update(example.get("categories") or [])
        path = str(example.get("path") or "")
        if (ROOT / "conformance").is_dir() and not (ROOT / path).is_file():
            errors.append("example %s path does not exist" % example.get("id"))
    if not REQUIRED_EXAMPLE_CATEGORIES <= categories:
        errors.append("examples do not cover %s" % sorted(
            REQUIRED_EXAMPLE_CATEGORIES - categories))
    if errors:
        raise ValueError("\n".join(errors))


# ---------------------------------------------------------------- versions


def parse_version(text: Any) -> tuple[int, int, int] | None:
    match = re.match(r"^v?(\d+)\.(\d+)(?:\.(\d+))?", str(text or "").strip())
    if not match:
        return None
    return int(match.group(1)), int(match.group(2)), int(match.group(3) or 0)


def skew_verdict(contract: Mapping[str, Any], service_version: Any) -> dict:
    minimum = str((contract.get("versioning") or {}).get("serviceMinimum") or "")
    parsed = parse_version(service_version)
    if parsed is None:
        verdict = "unknown"
    elif parsed < (parse_version(minimum) or (0, 0, 0)):
        verdict = "unsupported"
    else:
        verdict = "supported"
    return {
        "serviceVersion": "" if service_version is None else str(service_version),
        "minimum": minimum,
        "verdict": verdict,
    }


# ---------------------------------------------------------------- HTTP


class TransportError(RuntimeError):
    """No HTTP response arrived (DNS, TCP, TLS, or timeout)."""


class RetryPolicy:
    def __init__(self, max_attempts: int = 6, base_delay: float = 0.5,
                 max_delay: float = 30.0,
                 retry_statuses: Iterable[int] = RETRY_STATUSES):
        self.max_attempts = max(1, int(max_attempts))
        self.base_delay = float(base_delay)
        self.max_delay = float(max_delay)
        self.retry_statuses = frozenset(retry_statuses)

    def delay(self, attempt: int, retry_after: float | None = None,
              rng: random.Random | None = None) -> float:
        if retry_after is not None:
            return min(max(float(retry_after), 0.0), self.max_delay)
        ceiling = min(self.max_delay, self.base_delay * (2 ** max(0, attempt - 1)))
        return (rng or random).uniform(ceiling / 2, ceiling)


def _lower_headers(headers: Any) -> dict[str, str]:
    if headers is None:
        return {}
    items = headers.items() if hasattr(headers, "items") else headers
    return {str(key).lower(): str(value) for key, value in items}


def retry_after_seconds(headers: Mapping[str, str], body: Any = None) -> float | None:
    value = _lower_headers(headers).get("retry-after", "").strip()
    if value:
        try:
            return max(0.0, float(value))
        except ValueError:
            try:
                when = parsedate_to_datetime(value)
                return max(0.0, when.timestamp() - time.time())
            except (TypeError, ValueError):
                pass
    if isinstance(body, dict) and isinstance(body.get("retryAfterMs"), (int, float)):
        return max(0.0, float(body["retryAfterMs"]) / 1000.0)
    return None


class HttpResult:
    def __init__(self, status: int, headers: dict[str, str], body: bytes,
                 attempts: int, duration_ms: int):
        self.status = status
        self.headers = headers
        self.body = body
        self.attempts = attempts
        self.duration_ms = duration_ms
        self.json: Any = None
        if body:
            try:
                self.json = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                self.json = None


class Client:
    """A retrying JSON client that records request telemetry."""

    def __init__(self, base_url: str, *, timeout: float = 20.0,
                 policy: RetryPolicy | None = None,
                 sleep: Callable[[float], None] = time.sleep,
                 rng: random.Random | None = None,
                 headers: Mapping[str, str] | None = None):
        self.base_url = str(base_url).rstrip("/")
        self.timeout = timeout
        self.policy = policy or RetryPolicy()
        self.sleep = sleep
        self.rng = rng
        self.headers = dict(headers or {})
        self.telemetry = {
            "requests": 0, "retries": 0, "transportErrors": 0,
            "statusClasses": {}, "durationMs": 0,
        }

    def request(self, method: str, path: str, *, body: Any = None,
                headers: Mapping[str, str] | None = None,
                idempotent: bool | None = None) -> HttpResult:
        method = method.upper()
        if idempotent is None:
            idempotent = method in ("GET", "HEAD", "PUT", "DELETE")
        data = None
        merged = {"Accept": "application/json", "User-Agent": USER_AGENT}
        merged.update(self.headers)
        merged.update(headers or {})
        if body is not None:
            data = body if isinstance(body, bytes) else canonical_json(body)
            merged.setdefault("Content-Type", "application/json")
        started = time.monotonic()
        attempt = 0
        while True:
            attempt += 1
            self.telemetry["requests"] += 1
            request = urlrequest.Request(
                self.base_url + path, data=data, method=method, headers=merged)
            try:
                with urlrequest.urlopen(request, timeout=self.timeout) as response:
                    status = int(response.status)
                    response_headers = _lower_headers(response.headers)
                    raw = response.read()
            except urlerror.HTTPError as exc:
                status = int(exc.code)
                response_headers = _lower_headers(exc.headers)
                raw = exc.read()
            except (urlerror.URLError, OSError) as exc:
                self.telemetry["transportErrors"] += 1
                if idempotent and attempt < self.policy.max_attempts:
                    self.telemetry["retries"] += 1
                    self.sleep(self.policy.delay(attempt, None, self.rng))
                    continue
                self._finish(started)
                raise TransportError("%s %s: %s" % (method, path, exc)) from exc
            status_class = "%dxx" % (status // 100)
            classes = self.telemetry["statusClasses"]
            classes[status_class] = classes.get(status_class, 0) + 1
            retryable = status in self.policy.retry_statuses and (
                idempotent or status == 429)
            if retryable and attempt < self.policy.max_attempts:
                parsed = None
                try:
                    parsed = json.loads(raw.decode("utf-8")) if raw else None
                except (UnicodeDecodeError, ValueError):
                    parsed = None
                self.telemetry["retries"] += 1
                self.sleep(self.policy.delay(
                    attempt, retry_after_seconds(response_headers, parsed),
                    self.rng))
                continue
            return HttpResult(status, response_headers, raw, attempt,
                              self._finish(started))

    def get_json(self, path: str) -> Any:
        result = self.request("GET", path)
        if result.status != 200 or not isinstance(result.json, dict):
            raise TransportError("GET %s answered HTTP %d" % (path, result.status))
        return result.json

    def _finish(self, started: float) -> int:
        elapsed = int((time.monotonic() - started) * 1000)
        self.telemetry["durationMs"] += elapsed
        return elapsed


# ---------------------------------------------------------------- runner


def check_fields(payload: Any, required: Mapping[str, str]) -> list[str]:
    if not isinstance(payload, dict):
        return ["response is not a JSON object"]
    problems = []
    for field, kind in required.items():
        if field not in payload:
            problems.append("missing field %s" % field)
        elif not FIELD_TYPES[kind](payload[field]):
            problems.append("field %s is not %s" % (field, kind))
    return problems


def run_contract(contract: Mapping[str, Any], target: str, base_url: str, *,
                 client: Client | None = None,
                 allow_mutating: bool = False) -> dict:
    if target not in SERVICES:
        raise ValueError("target must be one of %s" % ", ".join(SERVICES))
    client = client or Client(base_url)
    deprecated = {item.get("id") for item in contract.get("deprecations") or []}
    results = []
    service_version = None
    for resource in (contract.get("interfaces") or {}).get("http") or []:
        if resource.get("service") != target:
            continue
        entry = {
            "id": resource["id"], "kind": resource["kind"],
            "method": resource["method"], "path": resource["path"],
        }
        if resource.get("mutating") and not allow_mutating:
            entry.update(status="skip", problems=[
                "mutating probe; pass --allow-mutating"])
            results.append(entry)
            continue
        try:
            response = client.request(
                resource["method"], resource["path"],
                body={} if resource["method"] == "POST" else None,
                idempotent=resource.get("idempotent"))
        except TransportError as exc:
            entry.update(status="fail", problems=[str(exc)])
            results.append(entry)
            continue
        problems = []
        if response.status not in resource["expectStatus"]:
            problems.append("HTTP %d not in %s" % (
                response.status, resource["expectStatus"]))
        problems.extend(check_fields(response.json, resource["requiredFields"]))
        payload = response.json if isinstance(response.json, dict) else {}
        items_field = resource.get("itemsField")
        if items_field and isinstance(payload.get(items_field), list):
            for index, item in enumerate(payload[items_field][:ITEM_CHECK_LIMIT]):
                for problem in check_fields(item, resource["itemFields"]):
                    problems.append("%s[%d]: %s" % (items_field, index, problem))
        for header, value in (resource.get("expectHeaders") or {}).items():
            actual = response.headers.get(header.lower(), "")
            if value.upper() not in [part.strip().upper() for part in actual.split(",")]:
                problems.append("header %s is %r, expected %s" % (
                    header, actual, value))
        if resource.get("versionField") and payload.get(resource["versionField"]):
            service_version = payload[resource["versionField"]]
        warnings = []
        if resource["id"] in deprecated or resource.get("stability") == "deprecated":
            warnings.append("resource is deprecated; see contract deprecations")
        entry.update(
            status="fail" if problems else "pass",
            httpStatus=response.status,
            attempts=response.attempts,
            durationMs=response.duration_ms,
            additionalFields=len(set(payload) - set(resource["requiredFields"])),
            problems=problems,
        )
        if warnings:
            entry["warnings"] = warnings
        results.append(entry)

    skew = skew_verdict(contract, service_version)
    results.append({
        "id": "version-skew", "kind": "skew",
        "status": "fail" if skew["verdict"] == "unsupported" else "pass",
        "problems": (
            ["service %s is below the supported minimum %s" % (
                skew["serviceVersion"], skew["minimum"])]
            if skew["verdict"] == "unsupported" else []),
    })
    passed = sum(1 for item in results if item["status"] == "pass")
    failed = sum(1 for item in results if item["status"] == "fail")
    skipped = sum(1 for item in results if item["status"] == "skip")
    return {
        "schema": REPORT_SCHEMA,
        "contract": {
            "id": contract["suite"]["id"],
            "version": contract["suite"]["version"],
        },
        "target": target,
        "baseUrl": redact_url(base_url),
        "generatedAt": _utc_now(),
        "client": {
            "tool": TOOL_VERSION,
            "python": platform.python_version(),
            "platform": platform.system().lower(),
        },
        "service": skew,
        "results": results,
        "summary": {
            "passed": passed, "failed": failed, "skipped": skipped,
            "passRate": round(passed / max(1, passed + failed), 4),
        },
        "telemetry": dict(client.telemetry),
    }


def discover(contract: Mapping[str, Any], target: str, base_url: str, *,
             client: Client | None = None) -> dict:
    report = run_contract(contract, target, base_url, client=client)
    return {
        "schema": CLI_SCHEMA,
        "command": "discover",
        "target": target,
        "baseUrl": report["baseUrl"],
        "contractVersion": report["contract"]["version"],
        "service": report["service"],
        "capabilities": {
            item["id"]: item["status"] == "pass"
            for item in report["results"]
            if item["kind"] == "resource" and item["status"] != "skip"
        },
    }


# ---------------------------------------------------------------- events


def make_event(source: str, event_type: str, subject: str, sequence: int,
               digest: str, data: Any, occurred_at: int | None = None) -> dict:
    identity = hashlib.sha256(
        ("%s\n%s\n%d\n%s" % (source, subject, sequence, digest)).encode("utf-8")
    ).hexdigest()[:32]
    return {
        "schema": EVENT_SCHEMA,
        "id": identity,
        "type": event_type,
        "subject": subject,
        "sequence": int(sequence),
        "digest": digest,
        "occurredAt": int(occurred_at if occurred_at is not None else _now_ms()),
        "source": source,
        "data": data,
    }


def validate_event(event: Any) -> dict:
    if not isinstance(event, dict) or event.get("schema") != EVENT_SCHEMA:
        raise ValueError("not a %s event" % EVENT_SCHEMA)
    for field in ("id", "type", "subject", "digest", "source"):
        if not isinstance(event.get(field), str) or (
                field != "digest" and not event.get(field)):
            raise ValueError("event field %s is invalid" % field)
    sequence = event.get("sequence")
    if not isinstance(sequence, int) or isinstance(sequence, bool) or sequence < 1:
        raise ValueError("event sequence must be a positive integer")
    return event


def catalog_snapshot(payload: Mapping[str, Any]) -> dict[str, dict]:
    snapshot: dict[str, dict] = {}
    for item in payload.get("repositories") or []:
        if not isinstance(item, dict):
            continue
        owner = str(item.get("owner") or "").strip()
        name = str(item.get("name") or "").strip()
        if not owner or not name:
            continue
        data = {"owner": owner, "name": name}
        data.update({field: item[field] for field in CATALOG_EVENT_FIELDS
                     if field in item})
        # The catalog is newest-first; a same-named mirror record later in
        # the list must not replace the canonical row.
        snapshot.setdefault(("%s/%s" % (owner, name)).lower(), {
            "kind": "repository", "data": data,
        })
    return snapshot


def daemon_snapshot(payload: Mapping[str, Any]) -> dict[str, dict]:
    node = str(payload.get("node") or "").strip()
    snapshot = {
        "node:" + node: {
            "kind": "mirror.node",
            "data": {
                "node": node,
                "version": payload.get("version"),
                "ready": payload.get("ready"),
                "lastSyncOk": payload.get("lastSyncOk"),
            },
        },
    }
    repositories = payload.get("repositories") or {}
    if isinstance(repositories, dict):
        for key in sorted(repositories):
            snapshot[str(key)] = {
                "kind": "mirror.repository",
                "data": {"repository": str(key), "state": repositories[key]},
            }
    return snapshot


def fetch_snapshot(client: Client, target: str) -> dict[str, dict]:
    if target == "hosted":
        return catalog_snapshot(client.get_json("/api/repositories"))
    return daemon_snapshot(client.get_json("/v1/status"))


class ChangeFeed:
    """Diff successive snapshots into per-subject sequenced events."""

    def __init__(self, source: str, state: Mapping[str, Any] | None = None):
        self.source = redact_url(source)
        self.state = dict(state) if state else {
            "schema": WATCH_STATE_SCHEMA, "source": self.source, "subjects": {},
        }
        if self.state.get("schema") != WATCH_STATE_SCHEMA:
            raise ValueError("watch state is not %s" % WATCH_STATE_SCHEMA)

    def observe(self, snapshot: Mapping[str, Mapping[str, Any]],
                now_ms: int | None = None) -> list[dict]:
        subjects = self.state["subjects"]
        events = []
        for subject in sorted(snapshot):
            entry = snapshot[subject]
            digest = hashlib.sha256(canonical_json(entry["data"])).hexdigest()
            prior = subjects.get(subject)
            if prior and prior["digest"] == digest and not prior.get("removed"):
                continue
            sequence = int(prior["sequence"]) + 1 if prior else 1
            subjects[subject] = {
                "sequence": sequence, "digest": digest,
                "kind": entry["kind"], "removed": False,
            }
            events.append(make_event(
                self.source, entry["kind"] + ".changed", subject, sequence,
                digest, entry["data"], now_ms))
        for subject in sorted(subjects):
            prior = subjects[subject]
            if subject in snapshot or prior.get("removed"):
                continue
            prior.update(sequence=int(prior["sequence"]) + 1, digest="",
                         removed=True)
            events.append(make_event(
                self.source, prior["kind"] + ".removed", subject,
                prior["sequence"], "", None, now_ms))
        return events


class EventLedger:
    """Apply at-least-once events exactly once, in per-subject order.

    accept() returns (action, payload) pairs:
      apply     – the event is the subject's next state; act on it now.
      duplicate – this event id was already accepted; acknowledge and drop.
      stale     – an older sequence arrived after a newer one; drop.
      buffered  – a later sequence arrived early; it waits for the gap.
      resync    – expire() gave up on a gap; payload names the subject and
                  missing range. The buffered event (full state) is applied
                  next, so state converges; re-read the resource over HTTP if
                  the skipped transitions matter.
    A subject's first accepted event is its baseline, whatever its sequence.
    """

    def __init__(self, *, gap_timeout: float = GAP_TIMEOUT_SECONDS,
                 max_pending: int = 256, seen_limit: int = 50000,
                 clock: Callable[[], float] = time.monotonic):
        self.gap_timeout = float(gap_timeout)
        self.max_pending = max(1, int(max_pending))
        self.seen_limit = max(1, int(seen_limit))
        self.clock = clock
        self.applied: dict[str, int] = {}
        self.state: dict[str, Any] = {}
        self.pending: dict[str, dict[int, tuple[dict, float]]] = {}
        self.seen: OrderedDict[str, None] = OrderedDict()

    def accept(self, event: Any) -> list[tuple[str, Any]]:
        validate_event(event)
        if event["id"] in self.seen:
            return [("duplicate", event)]
        self.seen[event["id"]] = None
        while len(self.seen) > self.seen_limit:
            self.seen.popitem(last=False)
        subject = event["subject"]
        sequence = event["sequence"]
        last = self.applied.get(subject)
        if last is None or sequence == last + 1:
            return self._apply_and_drain(event)
        if sequence <= last:
            return [("stale", event)]
        bucket = self.pending.setdefault(subject, {})
        bucket.setdefault(sequence, (event, self.clock()))
        actions: list[tuple[str, Any]] = [("buffered", event)]
        if len(bucket) > self.max_pending:
            actions.extend(self._skip_gap(subject))
        return actions

    def expire(self) -> list[tuple[str, Any]]:
        now = self.clock()
        actions: list[tuple[str, Any]] = []
        for subject in sorted(self.pending):
            bucket = self.pending.get(subject) or {}
            if bucket and now - min(arrived for _, arrived in bucket.values()) \
                    >= self.gap_timeout:
                actions.extend(self._skip_gap(subject))
        return actions

    def reset(self, subject: str | None = None) -> None:
        """Forget ordering so the next event becomes a baseline (stream resync)."""
        subjects = [subject] if subject else list(self.applied)
        for name in subjects:
            self.applied.pop(name, None)
            self.pending.pop(name, None)

    def snapshot(self) -> dict[str, Any]:
        return {subject: data for subject, data in sorted(self.state.items())
                if data is not None}

    def to_dict(self) -> dict:
        return {
            "applied": dict(self.applied),
            "state": dict(self.state),
            "seen": list(self.seen),
            "pending": [event for bucket in self.pending.values()
                        for event, _ in bucket.values()],
        }

    @classmethod
    def from_dict(cls, data: Mapping[str, Any], **options) -> "EventLedger":
        ledger = cls(**options)
        ledger.applied = {str(k): int(v) for k, v in (data.get("applied") or {}).items()}
        ledger.state = dict(data.get("state") or {})
        ledger.seen = OrderedDict((str(item), None) for item in data.get("seen") or [])
        now = ledger.clock()
        for event in data.get("pending") or []:
            validate_event(event)
            ledger.pending.setdefault(event["subject"], {})[event["sequence"]] = (
                event, now)
        return ledger

    def _apply_and_drain(self, event: dict) -> list[tuple[str, Any]]:
        subject = event["subject"]
        actions = [self._apply(event)]
        bucket = self.pending.get(subject) or {}
        while self.applied[subject] + 1 in bucket:
            actions.append(self._apply(bucket.pop(self.applied[subject] + 1)[0]))
        for sequence in [s for s in bucket if s <= self.applied[subject]]:
            bucket.pop(sequence)
        if not bucket:
            self.pending.pop(subject, None)
        return actions

    def _apply(self, event: dict) -> tuple[str, Any]:
        self.applied[event["subject"]] = event["sequence"]
        self.state[event["subject"]] = (
            None if event["type"].endswith(".removed") else event.get("data"))
        return ("apply", event)

    def _skip_gap(self, subject: str) -> list[tuple[str, Any]]:
        bucket = self.pending.get(subject) or {}
        if not bucket:
            return []
        lowest = min(bucket)
        first_missing = self.applied.get(subject, 0) + 1
        event, _ = bucket.pop(lowest)
        actions: list[tuple[str, Any]] = [("resync", {
            "subject": subject,
            "missing": [first_missing, lowest - 1],
            "resumeAt": lowest,
        })]
        actions.extend(self._apply_and_drain(event))
        return actions


# ---------------------------------------------------------------- webhooks


class WebhookVerificationError(ValueError):
    pass


def webhook_signature(secret: str | bytes, event_id: str, timestamp: int,
                      body: bytes) -> str:
    key = secret.encode("utf-8") if isinstance(secret, str) else secret
    message = ("%s\n%d\n%s\n" % (WEBHOOK_SCHEME, int(timestamp), event_id)).encode(
        "utf-8") + body
    return "v1=" + hmac.new(key, message, hashlib.sha256).hexdigest()


def webhook_headers(secret: str | bytes, event: Mapping[str, Any], body: bytes,
                    attempt: int, timestamp: int) -> dict[str, str]:
    return {
        "Content-Type": "application/json",
        "User-Agent": USER_AGENT,
        "ForkMesh-Webhook-Id": event["id"],
        "ForkMesh-Webhook-Timestamp": str(int(timestamp)),
        "ForkMesh-Webhook-Signature": webhook_signature(
            secret, event["id"], timestamp, body),
        "ForkMesh-Webhook-Attempt": str(int(attempt)),
        "ForkMesh-Event-Type": str(event.get("type") or ""),
    }


def verify_webhook(secrets: str | bytes | Iterable[str | bytes],
                   headers: Any, body: bytes, *, now: float | None = None,
                   tolerance: int = WEBHOOK_TOLERANCE_SECONDS) -> dict:
    """Return the verified event or raise WebhookVerificationError."""
    if isinstance(secrets, (str, bytes)):
        secrets = [secrets]
    lowered = _lower_headers(headers)
    event_id = lowered.get("forkmesh-webhook-id", "")
    timestamp_text = lowered.get("forkmesh-webhook-timestamp", "")
    signature = lowered.get("forkmesh-webhook-signature", "")
    if not event_id or not timestamp_text or not signature:
        raise WebhookVerificationError("missing_signature_headers")
    try:
        timestamp = int(timestamp_text)
    except ValueError:
        raise WebhookVerificationError("invalid_timestamp") from None
    current = time.time() if now is None else now
    if abs(current - timestamp) > tolerance:
        raise WebhookVerificationError("timestamp_outside_tolerance")
    provided = [part.strip() for part in signature.split(",")
                if part.strip().startswith("v1=")]
    if not any(
        hmac.compare_digest(webhook_signature(secret, event_id, timestamp, body),
                            candidate)
        for secret in secrets for candidate in provided
    ):
        raise WebhookVerificationError("signature_mismatch")
    try:
        event = validate_event(json.loads(body.decode("utf-8")))
    except (UnicodeDecodeError, ValueError) as exc:
        raise WebhookVerificationError("invalid_event") from exc
    if event["id"] != event_id:
        raise WebhookVerificationError("event_id_mismatch")
    return event


def deliver_event(url: str, secret: str | bytes, event: Mapping[str, Any], *,
                  policy: RetryPolicy | None = None,
                  sleep: Callable[[float], None] = time.sleep,
                  clock: Callable[[], float] = time.time,
                  timeout: float = 20.0,
                  rng: random.Random | None = None) -> dict:
    validate_event(event)
    policy = policy or RetryPolicy()
    body = canonical_json(event)
    status = None
    error = ""
    for attempt in range(1, policy.max_attempts + 1):
        request = urlrequest.Request(
            url, data=body, method="POST",
            headers=webhook_headers(secret, event, body, attempt, int(clock())))
        response_headers: dict[str, str] = {}
        try:
            with urlrequest.urlopen(request, timeout=timeout) as response:
                status = int(response.status)
                response.read()
        except urlerror.HTTPError as exc:
            status = int(exc.code)
            response_headers = _lower_headers(exc.headers)
            exc.read()
        except (urlerror.URLError, OSError) as exc:
            status = None
            error = str(exc)
        result = {"id": event["id"], "attempts": attempt, "status": status}
        if status is not None and 200 <= status < 300:
            return dict(result, delivered=True)
        if status == 410:
            return dict(result, delivered=False, reason="subscription_gone")
        if status is not None and status not in policy.retry_statuses:
            return dict(result, delivered=False, reason="rejected_%d" % status)
        if attempt < policy.max_attempts:
            sleep(policy.delay(
                attempt,
                retry_after_seconds(response_headers) if status else None, rng))
    result = {"id": event["id"], "attempts": policy.max_attempts,
              "status": status, "delivered": False, "reason": "retries_exhausted"}
    if error:
        result["error"] = error
    return result


def read_events(lines: Iterable[str]) -> Iterable[dict]:
    """Yield events from NDJSON: bare events or dead-letter records."""
    for line in lines:
        line = line.strip()
        if not line:
            continue
        record = json.loads(line)
        if isinstance(record, dict) and record.get("schema") == CLI_SCHEMA:
            record = record.get("event")
        yield validate_event(record)


# ---------------------------------------------------------------- fixture


FIXTURE_VERSIONS = {
    "previous": "0.8.0",
    "current": "0.8.8",
    "next": "0.9.0",
    "broken": "0.8.8",
    "unsupported": "0.7.4",
}


class FixtureState:
    """Mutable state behind the offline fixture service."""

    def __init__(self, profile: str = "current"):
        if profile not in FIXTURE_VERSIONS:
            raise ValueError("unknown fixture profile %s" % profile)
        self.profile = profile
        self.version = FIXTURE_VERSIONS[profile]
        self.lock = threading.Lock()
        self.ready = True
        self.sync_pending = False
        self.faults: dict[str, list[tuple[int, str | None]]] = {}
        self.requests: list[tuple[str, str]] = []
        self.repositories: "OrderedDict[str, dict]" = OrderedDict()
        self.node_repositories = {"forkmesh/forkmesh": "a" * 64}
        self.set_repository("forkmesh", "forkmesh",
                            description="Decentralized code collaboration")
        self.set_repository("example", "hello", description="Hello, mesh")

    def set_repository(self, owner: str, name: str, **fields: Any) -> None:
        key = ("%s/%s" % (owner, name)).lower()
        with self.lock:
            record = self.repositories.get(key) or {
                "owner": owner, "name": name, "description": "",
                "visibility": "public", "commit": "0" * 40,
                "stateHash": "0" * 64,
                "cloneUrl": "https://forkmesh.com/%s/%s" % (owner, name),
                "liveHost": True, "cloneOnline": True, "termsFlagged": False,
            }
            record.update(fields)
            record["updatedAt"] = _utc_now()
            self.repositories[key] = record
            self.repositories.move_to_end(key, last=False)

    def remove_repository(self, owner: str, name: str) -> None:
        with self.lock:
            self.repositories.pop(("%s/%s" % (owner, name)).lower(), None)

    def inject(self, path: str, *statuses: int | tuple[int, str]) -> None:
        with self.lock:
            queue = self.faults.setdefault(path, [])
            for item in statuses:
                queue.append(item if isinstance(item, tuple) else (item, None))


class _FixtureHandler(BaseHTTPRequestHandler):
    server_version = "ForkMeshFixture/1"

    def log_message(self, *_args: Any) -> None:
        return

    def do_GET(self) -> None:  # noqa: N802 - http.server API
        self._dispatch("GET")

    def do_HEAD(self) -> None:  # noqa: N802
        self._dispatch("HEAD")

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length") or 0)
        if length:
            self.rfile.read(length)
        self._dispatch("POST")

    def _send(self, status: int, payload: Any, headers: Mapping[str, str] | None = None) -> None:
        body = canonical_json(payload)
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _dispatch(self, method: str) -> None:
        state: FixtureState = self.server.state  # type: ignore[attr-defined]
        path = urlsplit(self.path).path
        with state.lock:
            state.requests.append((method, path))
            queue = state.faults.get(path)
            fault = queue.pop(0) if queue else None
        if fault:
            status, retry_after = fault
            headers = {"Retry-After": retry_after} if retry_after is not None else None
            return self._send(status, {"error": "injected_fault"}, headers)
        profile = state.profile
        now = _now_ms()
        if path == "/api/version" and method in ("GET", "HEAD"):
            payload: dict[str, Any] = {
                "ok": True, "rev": "fixture", "worker": "app",
                "version": state.version, "now": now,
            }
            if profile != "previous":
                payload["deployFingerprint"] = "f" * 64
            if profile == "next":
                payload["region"] = "fixture"
            if profile == "broken":
                payload.pop("rev")
            return self._send(200, payload)
        if path == "/api/openapi.json":
            return self._send(200, {
                "openapi": "3.0.3",
                "info": {"title": "ForkMesh API", "version": state.version},
                "paths": {"/api/version": {"get": {"summary": "version"}}},
            })
        if path == "/api/metrics/endpoints":
            return self._send(200, {"ok": True, "endpoints": [
                {"method": "GET", "template": "/api/version"},
                {"method": "GET", "template": "/api/repositories"},
            ]})
        if path == "/api/status":
            return self._send(200, {"ok": True, "now": now, "systems": [
                {"id": "website", "status": "operational"},
            ]})
        if path == "/api/mirrors/https":
            return self._send(200, {
                "ok": True, "protocol": "forkmesh-masked-proxy-v1",
                "registration": "forkmesh-https-endpoint-v1",
                "routerPublicKey": "fixture", "nonCustodial": True,
            })
        if path == "/api/repositories/search":
            with state.lock:
                results = [{
                    "owner": record.get("owner"), "name": record.get("name"),
                    "clone": {"https": "https://forkmesh.com/%s/%s" % (
                        record.get("owner"), record.get("name")),
                        "requiresAccount": False},
                } for record in state.repositories.values()
                    if record.get("visibility") == "public"]
            return self._send(200, {
                "ok": True, "view": "updated", "total": len(results),
                "results": results, "nextCursor": None,
            })
        if path == "/api/repositories":
            with state.lock:
                repositories = []
                for record in state.repositories.values():
                    item = dict(record)
                    # Volatile telemetry changes on every read, like live nodes.
                    item["cpuPercent"] = random.randint(0, 100)
                    if profile == "next":
                        item["topics"] = []
                    repositories.append(item)
            return self._send(200, {
                "ok": True, "repositories": repositories,
                "sshGatewayConfigured": False, "sshTransport": "not-configured",
            })
        if re.fullmatch(r"/api/chat/webhooks/[0-9a-f]{64}/?", path):
            if method != "POST":
                return self._send(405, {"error": "method_not_allowed"},
                                  {"Allow": "POST"})
            return self._send(404, {"error": "not_found"})
        if path in ("/healthz", "/readyz", "/v1/status"):
            if profile == "broken" and path == "/v1/status":
                return self._send(500, {"ok": False, "error": "fixture failure"})
            with state.lock:
                status = {
                    "ok": True, "ready": state.ready, "node": "fixture-node",
                    "version": state.version, "uptimeSeconds": 42,
                    "processes": {"gateway": True}, "restarts": {"gateway": 0},
                    "repositories": dict(state.node_repositories),
                    "lastSyncOk": True, "syncCount": 3,
                    "runtime": {"goroutines": 12, "heapBytes": 1, "totalAlloc": 1},
                }
            if path == "/healthz":
                return self._send(200, {"ok": True, "node": "fixture-node"})
            if path == "/readyz" and not status["ready"]:
                return self._send(503, status)
            return self._send(200, status)
        if path == "/v1/control/sync":
            if method != "POST":
                return self._send(405, {"ok": False, "error": "POST required"},
                                  {"Allow": "POST"})
            with state.lock:
                queued = not state.sync_pending
                state.sync_pending = True
            if queued:
                return self._send(202, {"ok": True, "queued": True})
            return self._send(202, {"ok": True, "queued": False,
                                    "message": "sync already requested"})
        return self._send(404, {"error": "not_found"})


class FixtureServer:
    """Serve FixtureState on a loopback port in a background thread."""

    def __init__(self, profile: str = "current", host: str = "127.0.0.1",
                 port: int = 0):
        self.state = FixtureState(profile)
        self.httpd = ThreadingHTTPServer((host, port), _FixtureHandler)
        self.httpd.daemon_threads = True
        self.httpd.state = self.state  # type: ignore[attr-defined]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        host, port = self.httpd.server_address[:2]
        return "http://%s:%d" % (host, port)

    def start(self) -> "FixtureServer":
        self.thread.start()
        return self

    def close(self) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()

    def __enter__(self) -> "FixtureServer":
        return self.start()

    def __exit__(self, *_exc: Any) -> None:
        self.close()


# ---------------------------------------------------------------- CLI


def _write_atomic(path: Path, data: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + "\n",
                         encoding="utf-8")
    os.replace(temporary, path)


def _resolve_target(args: argparse.Namespace) -> tuple[str, FixtureServer | None]:
    fixture = None
    if getattr(args, "fixture", None):
        fixture = FixtureServer(args.fixture).start()
        return fixture.base_url, fixture
    return args.base_url or DEFAULT_BASE_URLS[args.target], None


def _report_failures(target: str, base_url: str, failures: Iterable[str],
                     telemetry: Mapping[str, Any] | None = None) -> None:
    """Say on stderr why a contract or discover run exits non-zero.

    CI discards the JSON report on stdout, so without this a failed run leaves
    only its exit code behind (Actions run #334)."""
    prefix = "%s contract at %s" % (target, redact_url(base_url))
    for failure in failures:
        print("%s: %s" % (prefix, failure), file=sys.stderr)
    if telemetry and (telemetry.get("transportErrors") or telemetry.get("retries")):
        print("%s: telemetry %s" % (prefix, json.dumps(
            telemetry, sort_keys=True, separators=(",", ":"))), file=sys.stderr)


def _command_contract(args: argparse.Namespace, discover_only: bool) -> int:
    contract = load_contract(args.contract)
    validate_contract(contract)
    base_url, fixture = _resolve_target(args)
    try:
        if discover_only:
            line = discover(contract, args.target, base_url)
            emit(line)
            skew = line["service"]
            if skew["verdict"] == "unsupported":
                _report_failures(args.target, base_url, [
                    "service %s is below the supported minimum %s" % (
                        skew["serviceVersion"], skew["minimum"])])
                return EXIT_SKEW
            missing = [name for name, ok in line["capabilities"].items() if not ok]
            _report_failures(args.target, base_url, [
                "%s is unavailable" % name for name in missing])
            return EXIT_FAILED if missing else EXIT_OK
        report = run_contract(contract, args.target, base_url,
                              allow_mutating=args.allow_mutating)
    finally:
        if fixture:
            fixture.close()
    if args.output:
        _write_atomic(Path(args.output), report)
    emit({"schema": CLI_SCHEMA, "command": "contract", "report": report})
    failures = [
        "%s failed: %s" % (item["id"], "; ".join(item.get("problems") or [])
                           or "no reason recorded")
        for item in report["results"] if item["status"] == "fail"
    ]
    if failures:
        _report_failures(args.target, base_url, failures, report["telemetry"])
    if report["service"]["verdict"] == "unsupported":
        return EXIT_SKEW
    return EXIT_FAILED if report["summary"]["failed"] else EXIT_OK


def _command_watch(args: argparse.Namespace) -> int:
    base_url, fixture = _resolve_target(args)
    state_path = Path(args.state) if args.state else None
    state = None
    if state_path and state_path.is_file():
        state = json.loads(state_path.read_text(encoding="utf-8"))
    feed = ChangeFeed(base_url, state)
    interval = float(args.interval)
    if not fixture and not args.allow_fast_poll:
        interval = max(interval, MIN_POLL_SECONDS[args.target])
    client = Client(base_url)
    polls = 0
    failures = 0
    try:
        while True:
            polls += 1
            try:
                events = feed.observe(fetch_snapshot(client, args.target))
            except TransportError as exc:
                failures += 1
                emit({"schema": CLI_SCHEMA, "command": "watch",
                      "error": str(exc)}, sys.stderr)
                events = None
            if events is not None:
                failures = 0
                for event in events:
                    emit(event)
                # Persist after printing: a crash in between re-emits the same
                # ids, which consumers deduplicate, instead of losing events.
                if state_path:
                    _write_atomic(state_path, feed.state)
            if args.once or (args.max_polls and polls >= args.max_polls):
                return EXIT_FAILED if events is None else EXIT_OK
            time.sleep(interval if not failures else min(
                300.0, interval * (2 ** min(failures, 4))))
    finally:
        if fixture:
            fixture.close()


def _command_deliver(args: argparse.Namespace) -> int:
    secret = os.environ.get(args.secret_env, "")
    if not secret:
        print("set the webhook secret in $%s" % args.secret_env, file=sys.stderr)
        return EXIT_USAGE
    policy = RetryPolicy(max_attempts=args.max_attempts)
    source = sys.stdin if args.input == "-" else open(args.input, encoding="utf-8")
    dead_letters = []
    try:
        events = list(read_events(source))
    finally:
        if source is not sys.stdin:
            source.close()
    undelivered = 0
    for event in events:
        result = deliver_event(args.url, secret, event, policy=policy)
        emit(dict(result, schema=CLI_SCHEMA, command="deliver"))
        if not result["delivered"]:
            undelivered += 1
            dead_letters.append(dict(result, schema=CLI_SCHEMA,
                                     command="deliver", event=event))
    if args.dead_letter:
        path = Path(args.dead_letter)
        # Replaying a dead-letter file rewrites it with only what still fails.
        with open(path.with_name(path.name + ".tmp"), "w", encoding="utf-8") as handle:
            for record in dead_letters:
                handle.write(json.dumps(record, sort_keys=True) + "\n")
        os.replace(path.with_name(path.name + ".tmp"), path)
    return EXIT_FAILED if undelivered else EXIT_OK


def _command_fixture(args: argparse.Namespace) -> int:
    server = FixtureServer(args.profile, args.host, args.port).start()
    emit({"schema": CLI_SCHEMA, "command": "fixture", "profile": args.profile,
          "baseUrl": server.base_url})
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        return EXIT_OK
    finally:
        server.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)

    validate = commands.add_parser("validate", help="validate a contract manifest")
    validate.add_argument("--contract")

    for name in ("discover", "contract", "watch"):
        command = commands.add_parser(name)
        command.add_argument("--target", choices=SERVICES, required=True)
        command.add_argument("--base-url", default="")
        command.add_argument("--fixture", choices=sorted(FIXTURE_VERSIONS),
                             help="run against an in-process offline fixture")
        if name != "watch":
            command.add_argument("--contract")
        if name == "contract":
            command.add_argument("--allow-mutating", action="store_true")
            command.add_argument("--output")
        if name == "watch":
            command.add_argument("--state")
            command.add_argument("--interval", type=float, default=60.0)
            command.add_argument("--once", action="store_true")
            command.add_argument("--max-polls", type=int, default=0)
            command.add_argument("--allow-fast-poll", action="store_true")

    deliver = commands.add_parser("deliver")
    deliver.add_argument("--url", required=True)
    deliver.add_argument("--input", default="-")
    deliver.add_argument("--dead-letter")
    deliver.add_argument("--secret-env", default="FORKMESH_WEBHOOK_SECRET")
    deliver.add_argument("--max-attempts", type=int, default=6)

    fixture = commands.add_parser("fixture")
    fixture.add_argument("--profile", choices=sorted(FIXTURE_VERSIONS),
                         default="current")
    fixture.add_argument("--host", default="127.0.0.1")
    fixture.add_argument("--port", type=int, default=0)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "validate":
            validate_contract(load_contract(args.contract))
            emit({"schema": CLI_SCHEMA, "command": "validate", "ok": True})
            return EXIT_OK
        if args.command in ("discover", "contract"):
            return _command_contract(args, args.command == "discover")
        if args.command == "watch":
            return _command_watch(args)
        if args.command == "deliver":
            return _command_deliver(args)
        return _command_fixture(args)
    except (FileNotFoundError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    raise SystemExit(main())
