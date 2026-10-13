"""Pure validation helpers for profile contribution snapshots.

The desktop publisher signs the SHA-256 digest of the exact decoded snapshot
bytes. This module therefore keeps decoding and validation independent of the
Worker runtime and never substitutes reserialized JSON bytes for the payload.
"""

import base64
import hashlib
import json
import re
import time
import unicodedata
from collections.abc import Mapping
from datetime import date, datetime, timedelta, timezone


MAX_CONTRIBUTION_PAYLOAD_ENCODED = 64 * 1024
MAX_CONTRIBUTION_DAYS = 2048
MAX_CONTRIBUTION_EXTENSIONS = 256
MAX_CONTRIBUTION_COUNT = 1_000_000
MAX_CONTRIBUTION_YEARS = 5

_MAX_HEAD_LENGTH = 64
_MAX_BRANCH_LENGTH = 120
_MAX_EXTENSION_LENGTH = 32
_MAX_REPO_SEGMENT_LENGTH = 80
_MAX_SAFE_INTEGER = (1 << 53) - 1

_BASE64URL_RE = re.compile(r"^[A-Za-z0-9_-]+$")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_DIGEST_RE = re.compile(r"^[0-9a-f]{64}$")
_EXTENSION_RE = re.compile(r"^[a-z0-9][a-z0-9+#_-]{0,31}$")
_REPO_SEGMENT_RE = re.compile(r"^[A-Za-z0-9._:-]+$")
_UPDATED_AT_RE = re.compile(r"^(?:0|[1-9][0-9]{0,15})$")

_SNAPSHOT_KEYS = {
    "version",
    "capturedAt",
    "head",
    "branch",
    "from",
    "through",
    "coverage",
    "days",
    "extensions",
    "fileCount",
}
_COVERAGE_KEYS = {"commits", "collaboration", "languages"}
_COVERAGE_VALUES = {"complete", "partial"}

_LANGUAGES = {
    "c": ("C", "#555555"),
    "h": ("C", "#555555"),
    "cc": ("C++", "#f34b7d"),
    "cpp": ("C++", "#f34b7d"),
    "cxx": ("C++", "#f34b7d"),
    "hh": ("C++", "#f34b7d"),
    "hpp": ("C++", "#f34b7d"),
    "hxx": ("C++", "#f34b7d"),
    "cs": ("C#", "#178600"),
    "go": ("Go", "#00ADD8"),
    "html": ("HTML", "#e34c26"),
    "htm": ("HTML", "#e34c26"),
    "css": ("CSS", "#563d7c"),
    "java": ("Java", "#b07219"),
    "js": ("JavaScript", "#f1e05a"),
    "mjs": ("JavaScript", "#f1e05a"),
    "cjs": ("JavaScript", "#f1e05a"),
    "jsx": ("JavaScript", "#f1e05a"),
    "json": ("JSON", "#959595"),
    "kt": ("Kotlin", "#A97BFF"),
    "kts": ("Kotlin", "#A97BFF"),
    "md": ("Markdown", "#083fa1"),
    "markdown": ("Markdown", "#083fa1"),
    "mdown": ("Markdown", "#083fa1"),
    "mkdn": ("Markdown", "#083fa1"),
    "php": ("PHP", "#4F5D95"),
    "py": ("Python", "#3572A5"),
    "pyw": ("Python", "#3572A5"),
    "rb": ("Ruby", "#701516"),
    "rs": ("Rust", "#dea584"),
    "sh": ("Shell", "#89e051"),
    "bash": ("Shell", "#89e051"),
    "zsh": ("Shell", "#89e051"),
    "fish": ("Shell", "#89e051"),
    "sql": ("SQL", "#e38c00"),
    "swift": ("Swift", "#F05138"),
    "ts": ("TypeScript", "#3178c6"),
    "tsx": ("TypeScript", "#3178c6"),
    "mts": ("TypeScript", "#3178c6"),
    "cts": ("TypeScript", "#3178c6"),
    "yaml": ("YAML", "#cb171e"),
    "yml": ("YAML", "#cb171e"),
}
_OTHER_LANGUAGE = ("Other", "#8b949e")
_LANGUAGE_COLORS = {
    display_name: color
    for display_name, color in _LANGUAGES.values()
}
_LANGUAGE_COLORS[_OTHER_LANGUAGE[0]] = _OTHER_LANGUAGE[1]
_ACTIVITY_KINDS = ("commits", "issues", "pulls", "reviews", "repositories")


def _invalid(message):
    raise ValueError(message)


def _decode_base64url(value):
    if not isinstance(value, str) or not value:
        _invalid("base64url value must be a nonempty string")
    if not _BASE64URL_RE.fullmatch(value) or len(value) % 4 == 1:
        _invalid("invalid unpadded base64url value")
    try:
        raw = base64.b64decode(
            value + "=" * ((4 - len(value) % 4) % 4),
            altchars=b"-_",
            validate=True,
        )
    except (ValueError, TypeError):
        _invalid("invalid unpadded base64url value")
    canonical = base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")
    if canonical != value:
        _invalid("noncanonical base64url value")
    return raw


def _json_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            _invalid("duplicate JSON object key")
        value[key] = item
    return value


def _invalid_json_constant(_value):
    _invalid("invalid JSON numeric constant")


def _parse_json(raw):
    try:
        text = raw.decode("utf-8")
        value = json.loads(
            text,
            object_pairs_hook=_json_object,
            parse_constant=_invalid_json_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError):
        _invalid("invalid snapshot JSON")

    try:
        compact = json.dumps(
            value,
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
    except (RecursionError, TypeError, UnicodeEncodeError, ValueError):
        _invalid("invalid snapshot JSON")
    if compact != raw:
        _invalid("snapshot JSON must use compact encoding")
    return value


def _bounded_integer(value, name, maximum=MAX_CONTRIBUTION_COUNT):
    if type(value) is not int or value < 0 or value > maximum:
        _invalid("%s must be a bounded nonnegative integer" % name)
    return value


def _parse_date(value, name):
    if not isinstance(value, str) or not _DATE_RE.fullmatch(value):
        _invalid("%s must be an ISO UTC date" % name)
    try:
        return date.fromisoformat(value)
    except ValueError:
        _invalid("%s must be an ISO UTC date" % name)


def _utc_date_from_ms(value, name):
    _bounded_integer(value, name, _MAX_SAFE_INTEGER)
    try:
        return (
            datetime(1970, 1, 1, tzinfo=timezone.utc)
            + timedelta(milliseconds=value)
        ).date()
    except OverflowError:
        _invalid("%s is outside the supported timestamp range" % name)


def _latest_date_after_years(value, years):
    target_year = value.year + years
    if target_year > date.max.year:
        return date.max
    try:
        return value.replace(year=target_year)
    except ValueError:
        return value.replace(year=target_year, day=28)


def _bounded_string(value, name, maximum):
    if not isinstance(value, str) or not (1 <= len(value) <= maximum):
        _invalid("%s must be a bounded nonempty string" % name)
    if any(unicodedata.category(char)[0] in {"C", "Z"} for char in value):
        _invalid("%s contains invalid characters" % name)
    return value


def _actor_key(value):
    if not isinstance(value, str) or len(value) != 43:
        _invalid("actorPublicKey must be a 32-byte Ed25519 public key")
    if len(_decode_base64url(value)) != 32:
        _invalid("actorPublicKey must be a 32-byte Ed25519 public key")
    return value


def _normalized_extension(value):
    if not isinstance(value, str) or value != value.strip():
        _invalid("extension must be a string")
    normalized = value[1:] if value.startswith(".") else value
    normalized = normalized.lower()
    if (not normalized or len(normalized) > _MAX_EXTENSION_LENGTH or
            not _EXTENSION_RE.fullmatch(normalized)):
        _invalid("invalid extension")
    return normalized


def _validate_coverage(value):
    if not isinstance(value, dict) or set(value) != _COVERAGE_KEYS:
        _invalid("coverage must contain the exact contract fields")
    if any(not isinstance(item, str) or item not in _COVERAGE_VALUES
           for item in value.values()):
        _invalid("invalid coverage value")


def _validate_days(rows, from_date, through_date):
    if not isinstance(rows, list) or len(rows) > MAX_CONTRIBUTION_DAYS:
        _invalid("days must be a bounded array")
    previous = None
    for row in rows:
        if not isinstance(row, list) or len(row) != 6:
            _invalid("invalid day row")
        row_date = _parse_date(row[0], "day date")
        actor = _actor_key(row[1])
        if row_date < from_date or row_date > through_date:
            _invalid("day row is outside snapshot bounds")
        for index, kind in enumerate(
                ("commits", "issues", "pulls", "reviews"), start=2):
            _bounded_integer(row[index], kind)
        key = (row[0], actor)
        if previous is not None and key <= previous:
            _invalid("day rows must be strictly sorted and unique")
        previous = key


def _validate_extensions(rows):
    if not isinstance(rows, list) or len(rows) > MAX_CONTRIBUTION_EXTENSIONS:
        _invalid("extensions must be a bounded array")
    previous = None
    for row in rows:
        if not isinstance(row, list) or len(row) != 3:
            _invalid("invalid extension row")
        extension = _normalized_extension(row[0])
        _bounded_integer(row[1], "extension bytes")
        _bounded_integer(row[2], "extension files")
        if previous is not None and extension <= previous:
            _invalid("extension rows must be strictly sorted and unique")
        previous = extension


def decode_snapshot_payload(encoded, now_ms=None):
    """Return ``(snapshot, exact_raw_bytes, sha256_hex)`` after validation."""
    if not isinstance(encoded, str):
        _invalid("contributionPayload must be a string")
    if not encoded or len(encoded) > MAX_CONTRIBUTION_PAYLOAD_ENCODED:
        _invalid("contributionPayload is outside the encoded size limit")
    raw = _decode_base64url(encoded)
    snapshot = _parse_json(raw)

    if not isinstance(snapshot, dict) or set(snapshot) != _SNAPSHOT_KEYS:
        _invalid("snapshot must contain the exact version 1 fields")
    if type(snapshot["version"]) is not int or snapshot["version"] != 1:
        _invalid("unsupported snapshot version")

    if now_ms is None:
        now_ms = int(time.time() * 1000)
    _utc_date_from_ms(now_ms, "now_ms")
    captured_date = _utc_date_from_ms(snapshot["capturedAt"], "capturedAt")
    if snapshot["capturedAt"] > now_ms:
        _invalid("capturedAt cannot be in the future")

    _bounded_string(snapshot["head"], "head", _MAX_HEAD_LENGTH)
    _bounded_string(snapshot["branch"], "branch", _MAX_BRANCH_LENGTH)
    from_date = _parse_date(snapshot["from"], "from")
    through_date = _parse_date(snapshot["through"], "through")
    if from_date > through_date:
        _invalid("snapshot date range is reversed")
    if through_date > _latest_date_after_years(
            from_date, MAX_CONTRIBUTION_YEARS):
        _invalid("snapshot date range exceeds five years")
    if through_date > captured_date:
        _invalid("snapshot through date exceeds capturedAt")

    _validate_coverage(snapshot["coverage"])
    _validate_days(snapshot["days"], from_date, through_date)
    _validate_extensions(snapshot["extensions"])
    _bounded_integer(snapshot["fileCount"], "fileCount")
    return snapshot, raw, hashlib.sha256(raw).hexdigest()


def snapshot_signature_canonical(owner, repo, updated_at, digest):
    """Return the exact version 1 signature canonical bytes."""
    for name, value in (("owner", owner), ("repo", repo)):
        if (not isinstance(value, str) or
                not (1 <= len(value) <= _MAX_REPO_SEGMENT_LENGTH) or
                not _REPO_SEGMENT_RE.fullmatch(value)):
            _invalid("invalid %s for signature canonical form" % name)
    if (not isinstance(updated_at, str) or
            not _UPDATED_AT_RE.fullmatch(updated_at) or
            int(updated_at) > _MAX_SAFE_INTEGER):
        _invalid("invalid updated_at for signature canonical form")
    if not isinstance(digest, str) or not _DIGEST_RE.fullmatch(digest):
        _invalid("invalid digest for signature canonical form")
    return (
        "forkmesh-profile-contribution-v1\n"
        + owner + "\n"
        + repo + "\n"
        + updated_at + "\n"
        + digest
    ).encode("utf-8")


def contribution_date_range(query, now_ms=None):
    """Return an inclusive ISO date range limited to 366 days.

    An omitted ``from`` defaults to 365 inclusive days ending at ``to`` and
    clamps to ``date.min`` when the requested end is near the calendar floor.
    """
    if not isinstance(query, Mapping):
        _invalid("query must be a mapping")
    if set(query) - {"from", "to"}:
        _invalid("query contains unsupported contribution range fields")
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    today = _utc_date_from_ms(now_ms, "now_ms")

    to_date = _parse_date(query["to"], "to") if "to" in query else today
    from_date = (
        _parse_date(query["from"], "from")
        if "from" in query
        else date.fromordinal(max(date.min.toordinal(),
                                  to_date.toordinal() - 364))
    )
    if from_date > to_date:
        _invalid("contribution date range is reversed")
    if (to_date - from_date).days > 365:
        _invalid("contribution date range exceeds 366 inclusive days")
    return from_date.isoformat(), to_date.isoformat()


def language_for_extension(extension):
    """Return a stable ``(display_name, color)`` language bucket."""
    if not isinstance(extension, str):
        return _OTHER_LANGUAGE
    normalized = extension[1:] if extension.startswith(".") else extension
    return _LANGUAGES.get(normalized.lower(), _OTHER_LANGUAGE)


def language_color(language):
    """Return the stable color for a normalized stored language name."""
    if not isinstance(language, str):
        return _OTHER_LANGUAGE[1]
    return _LANGUAGE_COLORS.get(language, _OTHER_LANGUAGE[1])


def contribution_cacheable_range(from_value, to_value, now_ms=None):
    """Return true for the canonical rolling window or a full calendar year."""
    from_date = _parse_date(from_value, "from")
    to_date = _parse_date(to_value, "to")
    if now_ms is None:
        now_ms = int(time.time() * 1000)
    today = _utc_date_from_ms(now_ms, "now_ms")
    rolling_from = date.fromordinal(max(
        date.min.toordinal(), today.toordinal() - 364
    ))
    if from_date == rolling_from and to_date == today:
        return True
    return (
        from_date.year == to_date.year
        and from_date.month == 1
        and from_date.day == 1
        and to_date.month == 12
        and to_date.day == 31
    )


def aggregate_language_rows(rows):
    """Return exact top-five byte totals plus one deterministic Other bucket."""
    totals = {}
    for row in rows or []:
        language = row.get("language") if isinstance(row, Mapping) else None
        name = language if language in _LANGUAGE_COLORS else _OTHER_LANGUAGE[0]
        try:
            byte_count = int(row.get("bytes") or 0)
        except (TypeError, ValueError, AttributeError):
            byte_count = 0
        if byte_count > 0:
            totals[name] = totals.get(name, 0) + byte_count
    total_bytes = sum(totals.values())
    if total_bytes <= 0:
        return []

    other_bytes = totals.pop(_OTHER_LANGUAGE[0], 0)
    ranked = sorted(
        totals.items(),
        key=lambda item: (-item[1], item[0].casefold(), item[0]),
    )
    selected = ranked[:5]
    other_bytes += sum(byte_count for _name, byte_count in ranked[5:])
    if other_bytes:
        selected.append((_OTHER_LANGUAGE[0], other_bytes))
    return [
        {
            "name": name,
            "bytes": byte_count,
            "percentage": round(byte_count * 100.0 / total_bytes, 1),
            "color": language_color(name),
        }
        for name, byte_count in selected
    ]


def aggregate_coverage(rows, from_value):
    """Combine bounded active-project coverage into one public profile state."""
    rows = list(rows or [])
    if not rows:
        return {
            "status": "unavailable",
            "verifiedFrom": None,
            "updatedAt": 0,
            "missing": ["commits", "collaboration", "languages"],
        }

    verified_dates = []
    updated_at = 0
    missing = set()
    readable = 0
    for row in rows:
        try:
            updated_at = max(updated_at, int(row.get("captured_at") or 0))
        except (TypeError, ValueError, AttributeError):
            pass
        verified_from = row.get("verified_from") if isinstance(row, Mapping) else None
        try:
            verified_dates.append(_parse_date(verified_from, "verified_from"))
        except ValueError:
            pass
        coverage = row.get("coverage") if isinstance(row, Mapping) else None
        is_owned = bool(row.get("is_owned")) if isinstance(row, Mapping) else False
        required = (
            ("commits", "collaboration", "languages")
            if is_owned else ("collaboration",)
        )
        if isinstance(coverage, Mapping):
            readable += 1
            for kind in required:
                if coverage.get(kind) != "complete":
                    missing.add(kind)
        else:
            missing.update(required)

    if not readable:
        return {
            "status": "partial",
            "verifiedFrom": None,
            "updatedAt": updated_at,
            "missing": ["commits", "collaboration", "languages"],
        }
    verified_from = max(verified_dates) if verified_dates else None
    requested_from = _parse_date(from_value, "from")
    if verified_from is None or verified_from > requested_from:
        missing.add("history")
    order = ("commits", "collaboration", "languages", "history")
    ordered_missing = [kind for kind in order if kind in missing]
    return {
        "status": "partial" if ordered_missing else "complete",
        "verifiedFrom": verified_from.isoformat() if verified_from else None,
        "updatedAt": updated_at,
        "missing": ordered_missing,
    }


def build_profile_contribution_response(
        profile, from_value, to_value, summary, day_rows, language_rows,
        recent_activity, coverage_rows):
    """Build the exact public profile contribution response contract."""
    type_totals = {
        kind: int((summary or {}).get(kind) or 0)
        for kind in _ACTIVITY_KINDS
    }
    days = []
    for row in sorted(day_rows or [], key=lambda item: item.get("day", "")):
        counts = {
            kind: int(row.get(kind) or 0)
            for kind in _ACTIVITY_KINDS
        }
        if any(counts.values()):
            days.append({"date": row.get("day", ""), **counts})
    return {
        "ok": True,
        "profile": profile,
        "range": {"from": from_value, "to": to_value},
        "total": sum(type_totals.values()),
        "repositoryCount": int((summary or {}).get("repository_count") or 0),
        "typeTotals": type_totals,
        "days": days,
        "languages": aggregate_language_rows(language_rows),
        "recentActivity": list(recent_activity or [])[:20],
        "coverage": aggregate_coverage(coverage_rows, from_value),
    }
