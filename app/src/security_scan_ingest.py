"""Validation and encrypted D1 storage helpers for repository security scans.

This module is intentionally free of Workers/Pyodide imports.  ``entry.py``
owns HTTP routing, repository authorization, encryption, and D1 bindings; this
module owns the strict public-artifact contract and bounded history behavior.
"""

from datetime import datetime, timezone
import hashlib
import hmac
import json
import re


MAX_INGEST_BYTES = 1024 * 1024
MAX_HISTORY_PER_REPOSITORY = 90
MAX_HISTORY_RESPONSE = 30
MAX_FINDINGS = 500
MAX_LIST_ITEMS = 100

STATUS_VALUES = frozenset({
    "Daily scan completed",
    "No known critical findings detected",
    "Review required",
    "Critical findings detected",
    "Scan failed",
    "Scan outdated",
    "Scope limited",
    "Human review completed",
})
COMPLETION_VALUES = frozenset({"completed", "scope_limited", "failed"})
SEVERITIES = ("critical", "high", "medium", "low", "info")
CATEGORIES = ("dependency", "secret", "static")
FINDING_STATUSES = frozenset({"unreviewed", "confirmed", "dismissed"})
_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._:-]{1,80}$")
_COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|unknown)$")
_HEX_16_RE = re.compile(r"^[0-9a-f]{16}$")
_HEX_64_RE = re.compile(r"^[0-9a-f]{64}$")
_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")


class ScanEnvelopeError(ValueError):
    """Raised when an ingest body is not the documented public-safe schema."""


def _fail(field):
    raise ScanEnvelopeError("invalid security scan envelope field: " + field)


def _object(value, field, required, optional=()):
    if not isinstance(value, dict):
        _fail(field)
    keys = set(value)
    required = set(required)
    allowed = required | set(optional)
    if not required.issubset(keys) or not keys.issubset(allowed):
        _fail(field)
    return value


def _string(value, field, max_length, *, minimum=0, allowed=None):
    if not isinstance(value, str):
        _fail(field)
    if len(value) < minimum or len(value) > max_length or _CONTROL_RE.search(value):
        _fail(field)
    if allowed is not None and value not in allowed:
        _fail(field)
    return value


def _integer(value, field, *, minimum=0, maximum=10_000_000):
    if type(value) is not int or value < minimum or value > maximum:
        _fail(field)
    return value


def _strings(value, field, *, maximum=MAX_LIST_ITEMS, item_max=500,
             minimum=0):
    if not isinstance(value, list) or not minimum <= len(value) <= maximum:
        _fail(field)
    return [
        _string(item, field + "[]", item_max)
        for item in value
    ]


def timestamp_ms(value, field="timestamp"):
    _string(value, field, 40, minimum=1)
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except (TypeError, ValueError):
        _fail(field)
    if parsed.tzinfo is None:
        _fail(field)
    try:
        return int(parsed.timestamp() * 1000)
    except (OverflowError, OSError, ValueError):
        _fail(field)


def received_at_iso(value):
    try:
        number = max(0, int(value))
        return datetime.fromtimestamp(
            number / 1000, tz=timezone.utc
        ).isoformat().replace("+00:00", "Z")
    except (OverflowError, OSError, TypeError, ValueError):
        return "1970-01-01T00:00:00Z"


def repository_parts(value):
    value = _string(value, "rich.repository.name", 161, minimum=3)
    pieces = value.split("/")
    if len(pieces) != 2 or not all(_SEGMENT_RE.fullmatch(p) for p in pieces):
        _fail("rich.repository.name")
    return pieces[0], pieces[1]


def _relative_path(value, field, max_length=500):
    value = _string(value, field, max_length)
    if (value.startswith(("/", "\\")) or "\\" in value
            or any(part == ".." for part in value.split("/"))):
        _fail(field)
    return value


def _validate_rich(rich, expected_repository=None):
    _object(
        rich,
        "rich",
        {
            "schemaVersion", "visibility", "generatedAt", "repository",
            "scanner", "policy", "scan", "status", "summary", "findings",
            "recommendations", "notices",
        },
    )
    if rich.get("schemaVersion") != 1:
        _fail("rich.schemaVersion")
    if rich.get("visibility") != {
        "public": True,
        "redacted": True,
        "containsSourceExcerpts": False,
        "containsSecretValues": False,
    }:
        _fail("rich.visibility")
    timestamp_ms(rich.get("generatedAt"), "rich.generatedAt")

    repository = _object(
        rich.get("repository"), "rich.repository", {"name", "commit"})
    owner, repo = repository_parts(repository.get("name"))
    if (expected_repository
            and repository.get("name", "").lower()
            != str(expected_repository).lower()):
        _fail("rich.repository.name")
    if not _COMMIT_RE.fullmatch(str(repository.get("commit") or "")):
        _fail("rich.repository.commit")

    scanner = _object(
        rich.get("scanner"), "rich.scanner", {"name", "version", "model"})
    _string(scanner.get("name"), "rich.scanner.name", 100, minimum=1)
    _string(scanner.get("version"), "rich.scanner.version", 80, minimum=1)
    model = _object(
        scanner.get("model"), "rich.scanner.model", {"used", "name", "version"})
    if model != {"used": False, "name": None, "version": None}:
        _fail("rich.scanner.model")

    policy = _object(
        rich.get("policy"), "rich.policy", {"id", "version", "path", "sha256"})
    _string(policy.get("id"), "rich.policy.id", 100, minimum=1)
    _string(policy.get("version"), "rich.policy.version", 80, minimum=1)
    _relative_path(policy.get("path"), "rich.policy.path", 300)
    if not _HEX_64_RE.fullmatch(str(policy.get("sha256") or "")):
        _fail("rich.policy.sha256")

    scan = _object(
        rich.get("scan"),
        "rich.scan",
        {
            "startedAt", "completedAt", "durationMs", "completion",
            "areasIncluded", "areasExcluded", "filesInspected",
            "filesExcluded", "dependencyInventoryCount",
            "dependencyAdvisorySource", "scopeLimitations",
        },
    )
    started = timestamp_ms(scan.get("startedAt"), "rich.scan.startedAt")
    completed = timestamp_ms(scan.get("completedAt"), "rich.scan.completedAt")
    if completed < started:
        _fail("rich.scan.completedAt")
    _integer(
        scan.get("durationMs"), "rich.scan.durationMs",
        maximum=24 * 60 * 60 * 1000,
    )
    _string(
        scan.get("completion"), "rich.scan.completion", 20,
        allowed=COMPLETION_VALUES,
    )
    _strings(scan.get("areasIncluded"), "rich.scan.areasIncluded", item_max=300)
    _strings(scan.get("areasExcluded"), "rich.scan.areasExcluded", item_max=500)
    _integer(scan.get("filesInspected"), "rich.scan.filesInspected")
    _integer(scan.get("filesExcluded"), "rich.scan.filesExcluded")
    _integer(
        scan.get("dependencyInventoryCount"),
        "rich.scan.dependencyInventoryCount",
    )
    _string(
        scan.get("dependencyAdvisorySource"),
        "rich.scan.dependencyAdvisorySource",
        10,
        allowed={"OSV", "offline"},
    )
    _strings(
        scan.get("scopeLimitations"), "rich.scan.scopeLimitations",
        maximum=50, item_max=500,
    )

    _string(rich.get("status"), "rich.status", 60, allowed=STATUS_VALUES)
    summary = _object(
        rich.get("summary"), "rich.summary",
        {"total", "bySeverity", "byCategory"},
    )
    total = _integer(summary.get("total"), "rich.summary.total",
                     maximum=MAX_FINDINGS)
    severity = _object(
        summary.get("bySeverity"), "rich.summary.bySeverity", SEVERITIES)
    severity_counts = {
        key: _integer(
            severity.get(key), "rich.summary.bySeverity." + key,
            maximum=MAX_FINDINGS,
        )
        for key in SEVERITIES
    }
    category = _object(
        summary.get("byCategory"), "rich.summary.byCategory", CATEGORIES)
    category_counts = {
        key: _integer(
            category.get(key), "rich.summary.byCategory." + key,
            maximum=MAX_FINDINGS,
        )
        for key in CATEGORIES
    }
    if sum(severity_counts.values()) != total:
        _fail("rich.summary.bySeverity")
    if sum(category_counts.values()) != total:
        _fail("rich.summary.byCategory")

    findings = rich.get("findings")
    if not isinstance(findings, list) or len(findings) > MAX_FINDINGS:
        _fail("rich.findings")
    actual_severity = {key: 0 for key in SEVERITIES}
    actual_category = {key: 0 for key in CATEGORIES}
    for index, finding in enumerate(findings):
        field = "rich.findings[%d]" % index
        _object(
            finding,
            field,
            {
                "id", "category", "severity", "ruleId", "path", "line",
                "summary", "recommendation", "evidence",
                "falsePositiveStatus",
            },
            {"advisoryIds"},
        )
        finding_id = str(finding.get("id") or "")
        if not _HEX_16_RE.fullmatch(finding_id):
            _fail(field + ".id")
        category_name = _string(
            finding.get("category"), field + ".category", 20,
            allowed=set(CATEGORIES),
        )
        severity_name = _string(
            finding.get("severity"), field + ".severity", 20,
            allowed=set(SEVERITIES),
        )
        _string(finding.get("ruleId"), field + ".ruleId", 120, minimum=1)
        _relative_path(finding.get("path"), field + ".path")
        line = finding.get("line")
        if line is not None:
            _integer(line, field + ".line", minimum=1)
        _string(finding.get("summary"), field + ".summary", 300, minimum=1)
        _string(
            finding.get("recommendation"), field + ".recommendation",
            500, minimum=1,
        )
        evidence = _object(
            finding.get("evidence"), field + ".evidence",
            {"redacted", "fingerprint"},
        )
        fingerprint = str(evidence.get("fingerprint") or "")
        if evidence.get("redacted") is not True:
            _fail(field + ".evidence.redacted")
        if not _HEX_16_RE.fullmatch(fingerprint) or fingerprint != finding_id:
            _fail(field + ".evidence.fingerprint")
        finding_status = _string(
            finding.get("falsePositiveStatus"),
            field + ".falsePositiveStatus",
            20,
            allowed=FINDING_STATUSES,
        )
        # Scanner artifacts are immutable observations, not human decisions.
        # Confirmed/dismissed states may only come from the separately
        # authorized, encrypted review overlay.
        if finding_status != "unreviewed":
            _fail(field + ".falsePositiveStatus")
        if "advisoryIds" in finding:
            _strings(
                finding.get("advisoryIds"), field + ".advisoryIds",
                maximum=20, item_max=100,
            )
        actual_severity[severity_name] += 1
        actual_category[category_name] += 1
    if len(findings) != total or actual_severity != severity_counts:
        _fail("rich.summary.bySeverity")
    if actual_category != category_counts:
        _fail("rich.summary.byCategory")

    _strings(
        rich.get("recommendations"), "rich.recommendations",
        maximum=100, item_max=500,
    )
    _strings(
        rich.get("notices"), "rich.notices",
        maximum=20, item_max=500, minimum=3,
    )
    return owner, repo


def _validate_clipboard(clipboard):
    _object(
        clipboard,
        "clipboard",
        {
            "schemaVersion", "status", "scannedAt", "scanner", "model",
            "modelVersion", "policy", "durationMs", "commitHash", "included",
            "excluded", "findings", "categories", "recommendations",
            "reviewStatus", "falsePositiveStatus", "publicRedaction", "scopeLimitations",
            "limitations",
        },
    )
    if clipboard.get("schemaVersion") != 1:
        _fail("clipboard.schemaVersion")
    _string(clipboard.get("status"), "clipboard.status", 60,
            allowed=STATUS_VALUES)
    timestamp_ms(clipboard.get("scannedAt"), "clipboard.scannedAt")
    _string(clipboard.get("scanner"), "clipboard.scanner", 181, minimum=1)
    _string(clipboard.get("model"), "clipboard.model", 100)
    _string(clipboard.get("modelVersion"), "clipboard.modelVersion", 80)
    _string(clipboard.get("policy"), "clipboard.policy", 300, minimum=1)
    _integer(
        clipboard.get("durationMs"), "clipboard.durationMs",
        maximum=24 * 60 * 60 * 1000,
    )
    if not _COMMIT_RE.fullmatch(str(clipboard.get("commitHash") or "")):
        _fail("clipboard.commitHash")
    _strings(clipboard.get("included"), "clipboard.included", item_max=300)
    _strings(clipboard.get("excluded"), "clipboard.excluded", item_max=500)

    findings = _object(
        clipboard.get("findings"), "clipboard.findings",
        {"critical", "high", "medium", "low", "informational"},
    )
    for key in ("critical", "high", "medium", "low", "informational"):
        _integer(findings.get(key), "clipboard.findings." + key,
                 maximum=MAX_FINDINGS)
    categories = _object(
        clipboard.get("categories"), "clipboard.categories",
        {"dependency", "secretDetection", "staticAnalysis"},
    )
    for key in ("dependency", "secretDetection", "staticAnalysis"):
        _integer(categories.get(key), "clipboard.categories." + key,
                 maximum=MAX_FINDINGS)
    _strings(
        clipboard.get("recommendations"), "clipboard.recommendations",
        maximum=100, item_max=500,
    )
    _string(
        clipboard.get("reviewStatus"), "clipboard.reviewStatus", 80,
        minimum=1,
    )
    review_counts = _object(
        clipboard.get("falsePositiveStatus"),
        "clipboard.falsePositiveStatus",
        {"unreviewed", "confirmed", "dismissed"},
    )
    for key in FINDING_STATUSES:
        _integer(
            review_counts.get(key),
            "clipboard.falsePositiveStatus." + key,
            maximum=MAX_FINDINGS,
        )
    if (
        int(review_counts["confirmed"]) != 0
        or int(review_counts["dismissed"]) != 0
    ):
        _fail("clipboard.falsePositiveStatus")
    if sum(int(review_counts[key]) for key in FINDING_STATUSES) != \
            sum(int(findings[key]) for key in findings):
        _fail("clipboard.falsePositiveStatus")
    if clipboard.get("publicRedaction") != {
        "sourceCode": True,
        "secrets": True,
        "sensitivePrompts": True,
        "exploitDetails": True,
    }:
        _fail("clipboard.publicRedaction")
    _strings(
        clipboard.get("scopeLimitations"), "clipboard.scopeLimitations",
        maximum=50, item_max=500,
    )
    _strings(
        clipboard.get("limitations"), "clipboard.limitations",
        maximum=20, item_max=500, minimum=3,
    )


def clipboard_from_rich(rich):
    """Derive the one public representation accepted by the ingest endpoint."""

    severity = rich["summary"]["bySeverity"]
    categories = rich["summary"]["byCategory"]
    model = rich["scanner"]["model"]
    review_counts = {key: 0 for key in FINDING_STATUSES}
    for finding in rich["findings"]:
        review_counts[finding["falsePositiveStatus"]] += 1
    return {
        "schemaVersion": 1,
        "status": rich["status"],
        "scannedAt": rich["scan"]["completedAt"],
        "scanner": (
            str(rich["scanner"]["name"]) + " "
            + str(rich["scanner"]["version"])
        ).strip(),
        "model": str(model.get("name") or ""),
        "modelVersion": str(model.get("version") or ""),
        "policy": (
            str(rich["policy"]["id"]) + "@" + str(rich["policy"]["version"])
            + " sha256:" + str(rich["policy"]["sha256"])
        ),
        "durationMs": int(rich["scan"]["durationMs"]),
        "commitHash": rich["repository"]["commit"],
        "included": list(rich["scan"]["areasIncluded"]),
        "excluded": list(rich["scan"]["areasExcluded"]),
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
        "recommendations": list(rich["recommendations"]),
        "reviewStatus": "Not reviewed",
        "falsePositiveStatus": review_counts,
        "publicRedaction": {
            "sourceCode": True,
            "secrets": True,
            "sensitivePrompts": True,
            "exploitDetails": True,
        },
        "scopeLimitations": list(rich["scan"]["scopeLimitations"]),
        "limitations": list(rich["notices"]),
    }


def validate_ingest_envelope(payload, expected_repository=None):
    """Validate both documented schemas and their cross-document consistency."""

    _object(
        payload,
        "envelope",
        {"schemaVersion", "type", "rich", "clipboard"},
    )
    if payload.get("schemaVersion") != 1:
        _fail("envelope.schemaVersion")
    if payload.get("type") != "forkmesh.security-scan-ingest":
        _fail("envelope.type")
    _validate_rich(payload.get("rich"), expected_repository)
    _validate_clipboard(payload.get("clipboard"))
    if payload.get("clipboard") != clipboard_from_rich(payload.get("rich")):
        _fail("clipboard")
    # Canonical round-trip creates a detached, JSON-only snapshot for storage.
    return json.loads(canonical_json(payload))


def canonical_json(value):
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def envelope_scan_id(envelope):
    return hashlib.sha256(
        canonical_json(envelope).encode("utf-8")
    ).hexdigest()


def bearer_secret_matches(authorization, expected):
    """Compare a Bearer credential at fixed digest length without logging it."""

    if not isinstance(authorization, str) or not isinstance(expected, str):
        return False
    scheme, separator, candidate = authorization.strip().partition(" ")
    if (
        separator != " "
        or scheme.lower() != "bearer"
        or len(expected) < 32
        or len(expected) > 4096
    ):
        return False
    if (
        not candidate
        or len(candidate) > 4096
        or any(character.isspace() or ord(character) < 33
               for character in expected + candidate)
    ):
        return False
    return hmac.compare_digest(
        hashlib.sha256(candidate.encode("utf-8")).digest(),
        hashlib.sha256(expected.encode("utf-8")).digest(),
    )


def distinct_bearer_secrets(first, second):
    """Fail closed unless two configured bearer capabilities are independent."""

    if not isinstance(first, str) or not isinstance(second, str):
        return False
    if not 32 <= len(first) <= 4096 or not 32 <= len(second) <= 4096:
        return False
    if any(
        character.isspace() or ord(character) < 33
        for character in first + second
    ):
        return False
    return not hmac.compare_digest(
        hashlib.sha256(first.encode("utf-8")).digest(),
        hashlib.sha256(second.encode("utf-8")).digest(),
    )


def history_limit(value):
    try:
        value = int(value)
    except (TypeError, ValueError):
        value = 10
    return max(1, min(value, MAX_HISTORY_RESPONSE))


async def store_scan(run, encrypt, repo_bi, envelope, received_at):
    """Upsert one encrypted scan and prune this repository to a fixed count."""

    scan_id = envelope_scan_id(envelope)
    rich = envelope["rich"]
    scanned_at = timestamp_ms(rich["scan"]["completedAt"])
    encrypted = await encrypt(envelope)
    await run(
        """INSERT INTO repo_security_scans
           (repo_bi, scan_id, scanned_at, received_at, data)
           VALUES (?,?,?,?,?)
           ON CONFLICT(repo_bi, scan_id) DO NOTHING""",
        repo_bi,
        scan_id,
        scanned_at,
        int(received_at),
        encrypted,
    )
    await run(
        """DELETE FROM repo_security_scans
           WHERE repo_bi=? AND scan_id NOT IN (
             SELECT scan_id FROM repo_security_scans
             WHERE repo_bi=?
             ORDER BY scanned_at DESC, received_at DESC, scan_id DESC
             LIMIT ?
           )""",
        repo_bi,
        repo_bi,
        MAX_HISTORY_PER_REPOSITORY,
    )
    return scan_id


async def load_scans(all_rows, decrypt, repo_bi, expected_repository, limit):
    """Load and revalidate encrypted rows; corrupt or mismatched rows stay hidden."""

    rows = await all_rows(
        """SELECT scan_id, received_at, data
           FROM repo_security_scans
           WHERE repo_bi=?
           ORDER BY scanned_at DESC, received_at DESC, scan_id DESC
           LIMIT ?""",
        repo_bi,
        history_limit(limit),
    )
    results = []
    for row in rows or []:
        try:
            envelope = await decrypt(row.get("data", ""))
            envelope = validate_ingest_envelope(
                envelope, expected_repository=expected_repository)
            scan_id = envelope_scan_id(envelope)
            if not hmac.compare_digest(
                    scan_id, str(row.get("scan_id") or "")):
                continue
            results.append({
                "scanId": scan_id,
                "receivedAt": received_at_iso(row.get("received_at")),
                "envelope": envelope,
            })
        except Exception:
            continue
    return results


def scan_projection(record, *, include_rich=False):
    projected = {
        "scanId": record["scanId"],
        "receivedAt": record["receivedAt"],
        "clipboard": record["envelope"]["clipboard"],
    }
    if include_rich:
        projected["rich"] = record["envelope"]["rich"]
    return projected


def valid_scan_id(value):
    return bool(_HEX_64_RE.fullmatch(str(value or "").strip().lower()))


def valid_finding_id(value):
    return bool(_HEX_16_RE.fullmatch(str(value or "").strip().lower()))


def normalize_finding_status(value):
    value = str(value or "").strip().lower()
    return value if value in FINDING_STATUSES else ""


def apply_review_overlays(record, reviews):
    """Apply encrypted human-review rows without mutating the scan artifact."""

    projected = json.loads(canonical_json(record))
    envelope = projected.get("envelope") or {}
    rich = envelope.get("rich") or {}
    findings = rich.get("findings")
    if not isinstance(findings, list):
        return projected
    scan_id = str(projected.get("scanId") or "")
    overlays = {}
    for review in reviews or []:
        if str(review.get("scanId") or "") != scan_id:
            continue
        finding_id = str(review.get("findingId") or "").lower()
        status = normalize_finding_status(review.get("status"))
        if not valid_finding_id(finding_id) or not status:
            continue
        reviewed_at = max(0, int(review.get("reviewedAt") or 0))
        previous = overlays.get(finding_id)
        if not previous or reviewed_at >= previous[1]:
            overlays[finding_id] = (status, reviewed_at)
    counts = {key: 0 for key in FINDING_STATUSES}
    for finding in findings:
        finding_id = str(finding.get("id") or "").lower()
        status = normalize_finding_status(
            overlays.get(
                finding_id,
                (finding.get("falsePositiveStatus"), 0),
            )[0]
        ) or "unreviewed"
        finding["falsePositiveStatus"] = status
        counts[status] += 1
    clipboard = envelope.get("clipboard")
    if isinstance(clipboard, dict):
        clipboard["falsePositiveStatus"] = counts
        reviewed = counts["confirmed"] + counts["dismissed"]
        clipboard["reviewStatus"] = (
            "Not reviewed"
            if reviewed == 0
            else "Human review completed (%d reviewed, %d unreviewed)"
            % (reviewed, counts["unreviewed"])
        )
    return projected
