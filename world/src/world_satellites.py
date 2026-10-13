"""Pure, bounded CelesTrak OMM helpers for the ForkMesh World sky.

Network scheduling, D1 persistence, and HTTP responses belong to ``entry.py``.
This module only accepts the fixed CelesTrak VISUAL feed, reduces it to the
fields needed by SGP4, and creates a deterministic public snapshot. Keeping the
policy here makes malformed or unexpectedly large upstream replies testable
without a Cloudflare runtime.
"""

from datetime import datetime, timezone
import hashlib
import json
import math
import re


CELESTRAK_VISUAL_OMM_URL = (
    "https://celestrak.org/NORAD/elements/"
    "gp.php?GROUP=VISUAL&FORMAT=JSON"
)
CELESTRAK_SOURCE_NAME = "CelesTrak"
CELESTRAK_GROUP = "VISUAL"
CELESTRAK_FORMAT = "OMM JSON"
SATELLITE_PROPAGATOR = "SGP4"

MAX_CELESTRAK_RESPONSE_BYTES = 512 * 1024
MAX_SATELLITE_RECORDS = 160
MAX_SATELLITE_NAME = 80
MAX_OBJECT_ID = 32
SNAPSHOT_SCHEMA_VERSION = 1
MAX_SAFE_INTEGER = 9_007_199_254_740_991

_CATALOG_ID_RE = re.compile(r"^[1-9][0-9]{0,8}$")
_OBJECT_ID_RE = re.compile(r"^[0-9A-Z-]{0,32}$")
_EPOCH_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}"
    r"(?:\.\d{1,6})?Z?$"
)
_CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")

_OUTPUT_FIELDS = (
    "OBJECT_NAME",
    "OBJECT_ID",
    "EPOCH",
    "MEAN_MOTION",
    "ECCENTRICITY",
    "INCLINATION",
    "RA_OF_ASC_NODE",
    "ARG_OF_PERICENTER",
    "MEAN_ANOMALY",
    "NORAD_CAT_ID",
    "BSTAR",
    "MEAN_MOTION_DOT",
    "MEAN_MOTION_DDOT",
)
_SOURCE = {
    "name": CELESTRAK_SOURCE_NAME,
    "url": CELESTRAK_VISUAL_OMM_URL,
    "group": CELESTRAK_GROUP,
    "format": CELESTRAK_FORMAT,
    "propagator": SATELLITE_PROPAGATOR,
}


class SatelliteDataError(ValueError):
    """Stable validation failure safe for a bounded operational status."""

    def __init__(self, code):
        self.code = str(code)
        super().__init__(self.code)


def _fail(code):
    raise SatelliteDataError(code)


def _clean_text(value, limit, required=False):
    if not isinstance(value, str):
        if required:
            _fail("invalid_satellite_text")
        return ""
    text = " ".join(_CONTROL_RE.sub("", value).split()).strip()
    if len(text) > limit:
        _fail("satellite_text_too_long")
    if required and not text:
        _fail("missing_satellite_text")
    return text


def _catalog_id(value):
    if isinstance(value, bool):
        _fail("invalid_catalog_id")
    if isinstance(value, int):
        text = str(value)
    elif isinstance(value, str):
        text = value.strip()
    else:
        _fail("invalid_catalog_id")
    if not _CATALOG_ID_RE.fullmatch(text):
        _fail("invalid_catalog_id")
    if int(text) > 999_999_999:
        _fail("invalid_catalog_id")
    # Keep this as text. In particular, never force six-to-nine digit catalog
    # numbers through the legacy five-character TLE representation.
    return text


def _finite_number(value, field):
    if isinstance(value, bool):
        _fail("invalid_" + field.lower())
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        _fail("invalid_" + field.lower())
    if not math.isfinite(number):
        _fail("invalid_" + field.lower())
    return number


def _bounded_number(record, field, minimum, maximum, maximum_exclusive=False):
    number = _finite_number(record.get(field), field)
    invalid_high = (
        number >= maximum if maximum_exclusive else number > maximum
    )
    if number < minimum or invalid_high:
        _fail("invalid_" + field.lower())
    return number


def _normalize_epoch(value):
    if not isinstance(value, str):
        _fail("invalid_epoch")
    raw = value.strip()
    if not _EPOCH_RE.fullmatch(raw):
        _fail("invalid_epoch")
    bare = raw[:-1] if raw.endswith("Z") else raw
    try:
        parsed = datetime.fromisoformat(bare)
    except (TypeError, ValueError, OverflowError):
        _fail("invalid_epoch")
    if parsed.year < 1957 or parsed.year > 2100:
        _fail("invalid_epoch")
    canonical = parsed.isoformat(timespec="microseconds")
    if "." in canonical:
        canonical = canonical.rstrip("0").rstrip(".")
    return canonical + "Z"


def _epoch_ms(value):
    try:
        parsed = datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except (TypeError, ValueError, OverflowError):
        _fail("invalid_epoch")
    return int(parsed.timestamp() * 1000)


def normalize_omm_record(record):
    """Return one canonical satellite.js-compatible OMM record."""
    if not isinstance(record, dict):
        _fail("invalid_satellite_record")

    name = _clean_text(
        record.get("OBJECT_NAME"), MAX_SATELLITE_NAME, required=True
    )
    object_id = _clean_text(record.get("OBJECT_ID"), MAX_OBJECT_ID)
    object_id = object_id.upper()
    if not _OBJECT_ID_RE.fullmatch(object_id):
        _fail("invalid_object_id")

    normalized = {
        "OBJECT_NAME": name,
        "OBJECT_ID": object_id,
        "EPOCH": _normalize_epoch(record.get("EPOCH")),
        "MEAN_MOTION": _bounded_number(
            record, "MEAN_MOTION", 0.000001, 20.0
        ),
        "ECCENTRICITY": _bounded_number(
            record, "ECCENTRICITY", 0.0, 1.0, maximum_exclusive=True
        ),
        "INCLINATION": _bounded_number(
            record, "INCLINATION", 0.0, 180.0
        ),
        "RA_OF_ASC_NODE": _bounded_number(
            record, "RA_OF_ASC_NODE", 0.0, 360.0
        ),
        "ARG_OF_PERICENTER": _bounded_number(
            record, "ARG_OF_PERICENTER", 0.0, 360.0
        ),
        "MEAN_ANOMALY": _bounded_number(
            record, "MEAN_ANOMALY", 0.0, 360.0
        ),
        "NORAD_CAT_ID": _catalog_id(record.get("NORAD_CAT_ID")),
        "BSTAR": _bounded_number(record, "BSTAR", -100.0, 100.0),
        "MEAN_MOTION_DOT": _bounded_number(
            record, "MEAN_MOTION_DOT", -10.0, 10.0
        ),
        "MEAN_MOTION_DDOT": _bounded_number(
            record, "MEAN_MOTION_DDOT", -10.0, 10.0
        ),
    }
    return {field: normalized[field] for field in _OUTPUT_FIELDS}


def normalize_visual_omm_records(records):
    """Validate, deduplicate, and sort one parsed VISUAL-group response."""
    if not isinstance(records, list):
        _fail("invalid_celestrak_payload")
    if not records:
        _fail("empty_celestrak_payload")
    if len(records) > MAX_SATELLITE_RECORDS:
        _fail("too_many_satellites")

    by_catalog_id = {}
    for value in records:
        record = normalize_omm_record(value)
        catalog_id = record["NORAD_CAT_ID"]
        previous = by_catalog_id.get(catalog_id)
        if (
            previous is None
            or _epoch_ms(record["EPOCH"]) > _epoch_ms(previous["EPOCH"])
        ):
            by_catalog_id[catalog_id] = record
    return sorted(
        by_catalog_id.values(),
        key=lambda record: int(record["NORAD_CAT_ID"]),
    )


def parse_visual_omm_json(raw):
    """Parse a raw upstream body with hard byte and record-count bounds."""
    if isinstance(raw, str):
        encoded = raw.encode("utf-8")
    elif isinstance(raw, (bytes, bytearray, memoryview)):
        encoded = bytes(raw)
    else:
        _fail("invalid_celestrak_body")
    if not encoded:
        _fail("empty_celestrak_body")
    if len(encoded) > MAX_CELESTRAK_RESPONSE_BYTES:
        _fail("celestrak_body_too_large")
    try:
        parsed = json.loads(
            encoded.decode("utf-8"),
            parse_constant=lambda _value: _fail("invalid_celestrak_number"),
        )
    except SatelliteDataError:
        raise
    except (UnicodeDecodeError, TypeError, ValueError, json.JSONDecodeError):
        _fail("invalid_celestrak_json")
    return normalize_visual_omm_records(parsed)


def normalize_snapshot_payload(payload):
    """Validate a stored/public snapshot and return its canonical projection."""
    if not isinstance(payload, dict):
        _fail("invalid_satellite_snapshot")
    if (
        payload.get("ok") is not True
        or payload.get("schemaVersion") != SNAPSHOT_SCHEMA_VERSION
    ):
        _fail("invalid_snapshot_schema")
    fetched_at = payload.get("fetchedAt")
    if (
        isinstance(fetched_at, bool)
        or not isinstance(fetched_at, int)
        or fetched_at <= 0
        or fetched_at > MAX_SAFE_INTEGER
    ):
        _fail("invalid_snapshot_time")
    if payload.get("source") != _SOURCE:
        _fail("invalid_snapshot_source")
    records = normalize_visual_omm_records(payload.get("satellites"))
    source_epoch = max(records, key=lambda item: _epoch_ms(item["EPOCH"]))[
        "EPOCH"
    ]
    canonical = {
        "ok": True,
        "schemaVersion": SNAPSHOT_SCHEMA_VERSION,
        "source": dict(_SOURCE),
        "fetchedAt": fetched_at,
        "sourceEpoch": source_epoch,
        "recordCount": len(records),
        "satellites": records,
    }
    if (
        payload.get("sourceEpoch") != canonical["sourceEpoch"]
        or payload.get("recordCount") != canonical["recordCount"]
    ):
        _fail("invalid_snapshot_summary")
    return canonical


def build_snapshot_payload(records, fetched_at_ms):
    """Build one deterministic public snapshot from normalized OMM records."""
    if (
        isinstance(fetched_at_ms, bool)
        or not isinstance(fetched_at_ms, int)
        or fetched_at_ms <= 0
        or fetched_at_ms > MAX_SAFE_INTEGER
    ):
        _fail("invalid_snapshot_time")
    normalized = normalize_visual_omm_records(records)
    source_epoch = max(
        normalized, key=lambda item: _epoch_ms(item["EPOCH"])
    )["EPOCH"]
    payload = {
        "ok": True,
        "schemaVersion": SNAPSHOT_SCHEMA_VERSION,
        "source": dict(_SOURCE),
        "fetchedAt": fetched_at_ms,
        "sourceEpoch": source_epoch,
        "recordCount": len(normalized),
        "satellites": normalized,
    }
    return normalize_snapshot_payload(payload)


def build_snapshot_from_json(raw, fetched_at_ms):
    """Parse one CelesTrak body and build its public snapshot."""
    return build_snapshot_payload(parse_visual_omm_json(raw), fetched_at_ms)


def serialize_snapshot_payload(payload):
    """Return compact D1 text and its SHA-256 digest."""
    normalized = normalize_snapshot_payload(payload)
    document = json.dumps(
        normalized, ensure_ascii=True, separators=(",", ":"), sort_keys=True
    )
    if len(document.encode("utf-8")) > MAX_CELESTRAK_RESPONSE_BYTES:
        _fail("satellite_snapshot_too_large")
    digest = hashlib.sha256(document.encode("utf-8")).hexdigest()
    return document, digest


def parse_snapshot_document(document):
    """Parse and canonicalize one compact snapshot read from D1."""
    if not isinstance(document, str):
        _fail("invalid_satellite_snapshot_document")
    encoded = document.encode("utf-8")
    if not encoded or len(encoded) > MAX_CELESTRAK_RESPONSE_BYTES:
        _fail("invalid_satellite_snapshot_document")
    try:
        payload = json.loads(
            document,
            parse_constant=lambda _value: _fail("invalid_snapshot_number"),
        )
    except SatelliteDataError:
        raise
    except (TypeError, ValueError, json.JSONDecodeError):
        _fail("invalid_satellite_snapshot_document")
    return normalize_snapshot_payload(payload)
